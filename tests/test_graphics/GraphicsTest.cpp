#include <vine/graphics/BuiltinShaders.hpp>
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
#include <vine/graphics/Texture.hpp>
#include <vine/graphics/MaterialManager.hpp>
#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/CameraMirror.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/FramePlan.hpp>
#include <vine/graphics/Ray.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/LightRig.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/RayIntersection.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/Buffer.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/Colorf.hpp>
#include <vine/math/Transform3.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <stdexcept>
#include <thread>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::String;
using vn::Colorf;
using vn::Color;
using vn::imaging::Image;
using vn::imaging::PixelFormat;
using vn::math::Vec2d;
using vn::math::Vec3d;
using vn::math::Mat4d;
using vn::math::Aabbd;
using vn::math::Aabbf;

namespace
{

/**
 * @brief Shape of the picture the tests' cameras draw into: a square strip.
 *
 * Camera no longer carries an aspect (the rectangle does - see Camera::projectionMatrix), so every call that
 * turns viewing into a picture has to state the shape of the picture it produces. The cameras built in this file
 * are used with a square strip unless a test says otherwise, which is the shape the expected numbers were
 * written against. A call whose camera is ORTHOGRAPHIC ignores it (an orthographic window is not a frustum to
 * fit, it is a rectangle in view space) and says so by naming this constant all the same.
 */
constexpr double kSceneAspect{ 1.0 };

/**
 * @brief Whether @p pass presents @p camera to the window: it draws through that camera and has no render target,
 * so it draws to the default framebuffer.
 *
 * This is the fact a presentation used to be INFERRED from (the removed RenderEngine::hasWindowPass, which asked
 * the engine whether any pass carried a camera and a null target). Which pass draws into which surface is now
 * declared on the surface (RenderSurface) - these tests ask the pass they got, about the fact itself.
 *
 * @param pass   Pass to describe.
 * @param camera Camera it should draw through.
 * @return true when the pass draws through the camera to the window.
 */
bool drawsToWindow(vn::graphics::RenderPass* pass, vn::graphics::Camera* camera)
{
    return pass != nullptr && pass->isEnabled() && pass->camera() == camera && pass->renderTarget() == nullptr;
}

/**
 * @brief Builds a pipeline for @p view and adopts it: the two steps a host does for a view.
 *
 * A view no longer carries a recipe - a pipeline IS the recipe (see RenderPipelineBuilder and
 * SceneView::setPipeline) - so a test that wants "this view, drawn this way" states the options here and hands the
 * result over. The builder is given the view's own camera and scene, which is what every caller in the app does:
 * the view owns both, the pipeline owns the passes.
 *
 * @param view    View the pipeline is built for; it adopts the result.
 * @param engine  Engine the passes are registered on.
 * @param options The build options (chain, mode, overlays; see PipelineOptions).
 * @return The pipeline that was built and adopted; null when the builder refused @p options.
 */
intrusive_ptr<Pipeline> buildFor(SceneView& view, RenderEngine* engine, const PipelineOptions& options = {})
{
    RenderPipelineBuilder builder(engine);
    builder.setCamera(view.camera());
    builder.setContent(view.scene());
    intrusive_ptr<Pipeline> pipeline = builder.build(options);
    if (pipeline != nullptr) {
        view.setPipeline(pipeline);
    }
    return pipeline;
}

/**
 * @brief Makes a source image that matches a texture's description.
 *
 * @param texture   Texture whose size, format and mip count the image is built to match.
 * @param mip_count Number of mip levels the image should have.
 * @return A new image matching the description.
 */
intrusive_ptr<const Image> sourceImage(const Texture& texture, int mip_count)
{
    return intrusive_ptr<const Image>(new Image(texture.width(), texture.height(), texture.format(), mip_count));
}

/**
 * @brief Builds a unit triangle mesh in the XY plane.
 *
 * Vertices: (0,0,0), (1,0,0), (0,1,0).
 */
intrusive_ptr<vn::geometry::TriangleMesh> makeUnitTriangle()
{
    auto mesh = intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    return mesh;
}

/**
 * @brief Converts a mesh to a geometry, asserting the conversion succeeded.
 *
 * Thin wrapper over the SDK converter so each test does not repeat the null
 * check; geometryFromShape() itself is covered directly by
 * GeometryTest.ConverterFillsBuffersFromTriangleMesh.
 *
 * @param mesh Source mesh.
 * @return New geometry holding the mesh's vertex data.
 */
intrusive_ptr<Geometry> geometryOf(const vn::geometry::Mesh& mesh)
{
    auto geom = geometryFromShape(mesh);
    EXPECT_NE(geom.get(), nullptr);
    return geom;
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
                                                const vn::String& name = {})
{
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto geom = geometryOf(*makeUnitTriangle());
    geom->setMaterial(std::move(material));
    geom->setName(name);
    node->setMatrix(vn::math::translate(position));
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
    auto geom = geometryOf(*makeUnitTriangle());
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
    auto geom = geometryOf(*makeUnitTriangle());
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
    cam.setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);
    // The shape the engine's own passes draw at (one camera, one picture shape): stated HERE, where the camera
    // is configured, because every consumer of the projection reads it off the camera.
    cam.setAspect(kSceneAspect);
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
    EXPECT_FALSE(commands[0].isTransparent());
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
    EXPECT_FALSE(commands[0].isTransparent());
    // Transparent drawn back-to-front.
    EXPECT_EQ(commands[1].geometry->name(), u8"trans-far");
    EXPECT_TRUE(commands[1].isTransparent());
    EXPECT_EQ(commands[2].geometry->name(), u8"trans-near");
    EXPECT_TRUE(commands[2].isTransparent());
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

TEST(SceneTest, OneCameraInTwoRectanglesIsTwoPicturesAndTwoWalks)
{
    // The shape of the picture is the CALLER's (a camera holds none - see Camera), and it reaches the frustum: the
    // same camera culls what sits off to the side for a wide rectangle and drops it for a square one. A caller that
    // culls with a shape its picture does not have drops what the rectangle shows, which is the whole reason the
    // shape is an argument here.
    Scene scene;
    auto  root = setIdentityRoot(scene);
    root->addChild(makeTriangleNode(Vec3d(7, 0, -3), nullptr, u8"off-to-the-side"));

    Camera cam;
    setupLookAtCamera(cam);  // eye (0,0,5), vertical fov 60: about +-4.6 wide at the object's distance

    scene.setContentFrame(1);
    const auto square = scene.collectRenderCommandsShared(&cam);
    EXPECT_TRUE(square->empty()) << "a square picture culls what sits off to the side";
    EXPECT_EQ(scene.contentCollectCount(), 1u);
    EXPECT_EQ(scene.contentCollectReuseCount(), 0u);

    // Changing the camera's SHAPE is changing the view (one camera, one picture shape): the projection encodes
    // it, so the memo cannot answer for one shape with another's walk.
    cam.setAspect(8.0 / 3.0);
    const auto wide = scene.collectRenderCommandsShared(&cam);
    ASSERT_EQ(wide->size(), 1u) << "a wide one through the same eye holds it";
    EXPECT_EQ(scene.contentCollectCount(), 2u) << "one walk per SHAPE the camera has been given";
    EXPECT_EQ(scene.contentCollectReuseCount(), 0u);

    // Asking again reuses each shape's own answer - and never the other's, because the shape is part of the view
    // the memo is keyed on.
    cam.setAspect(kSceneAspect);
    EXPECT_TRUE(scene.collectRenderCommandsShared(&cam)->empty()) << "the square shape's own answer";
    cam.setAspect(8.0 / 3.0);
    EXPECT_EQ(scene.collectRenderCommandsShared(&cam)->size(), 1u) << "and the wide shape's";
    EXPECT_EQ(scene.contentCollectCount(), 2u) << "both shapes are remembered: still two walks";
    EXPECT_EQ(scene.contentCollectReuseCount(), 2u);
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
    auto g1 = geometryOf(*makeUnitTriangle());
    g1->setName(u8"g1");
    auto g2 = geometryOf(*makeUnitTriangle());
    g2->setName(u8"g2");
    g1->setVisible(false);
    node->addChild(g1);
    node->addChild(g2);
    node->setMatrix(vn::math::translate(Vec3d(0, 0, -3)));
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
    EXPECT_TRUE(commands[0].isTransparent());
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
    EXPECT_TRUE(commands[0].isTransparent());
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
    EXPECT_TRUE(commands[0].isTransparent());
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
    const auto origin_in_view = view * vn::math::Point3d(0, 0, 0);
    EXPECT_NEAR(origin_in_view.x, 0.0, 1e-6);
    EXPECT_NEAR(origin_in_view.y, 0.0, 1e-6);
    EXPECT_NEAR(origin_in_view.z, -5.0, 1e-6);
}

TEST(CameraTest, ScreenToWorldRayCenter)
{
    Camera cam;
    cam.setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam.setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);
    // The picture's shape is the CAMERA's own state (one camera, one picture shape): 4:3 here.
    cam.setAspect(800.0 / 600.0);

    // Screen center maps to a ray through the target.
    Ray ray = cam.screenToWorldRay(Vec2d(0.5, 0.5));
    EXPECT_NEAR(ray.origin.x, 0.0, 1e-6);
    EXPECT_NEAR(ray.origin.y, 0.0, 1e-6);
    EXPECT_NEAR(ray.origin.z, 5.0, 1e-6);
    // Direction should point towards -Z.
    EXPECT_NEAR(ray.direction.z, -1.0, 1e-6);
    EXPECT_NEAR(ray.direction.x, 0.0, 1e-6);
}

TEST(CameraTest, TheOrthographicWindowIsKeptWhole)
{
    // Only the height used to survive setProjectionMatrixAsOrtho(), so a backend that rebuilds the
    // projection from the camera's parameters rendered a CENTRED window while the engine culled with the
    // off-centre one it was given: content outside the rendered view was kept, and content inside it ...
    // was culled. The window is the whole frustum in view space, so all four bounds are part of it.
    Camera cam;
    cam.setProjectionMatrixAsOrtho(-2.0, 6.0, -1.0, 3.0, 0.5, 100.0);

    EXPECT_EQ(cam.projectionType(), Camera::ProjectionType::Orthographic);
    EXPECT_NEAR(cam.orthographicLeft(), -2.0, 1e-9);
    EXPECT_NEAR(cam.orthographicRight(), 6.0, 1e-9);
    EXPECT_NEAR(cam.orthographicBottom(), -1.0, 1e-9);
    EXPECT_NEAR(cam.orthographicTop(), 3.0, 1e-9);
    EXPECT_NEAR(cam.orthographicHeight(), 4.0, 1e-9);

    // The stored window IS the projection: the matrix's own centre (what maps to NDC 0,0) is the
    // window's centre — ((left + right) / 2, (bottom + top) / 2) — and not the view axis.
    const auto window_centre = cam.projectionMatrix() * vn::math::Point3d(2.0, 1.0, -0.5);
    EXPECT_NEAR(window_centre.x, 0.0, 1e-9) << "x = 2 is the centre of a [-2, 6] window";
    EXPECT_NEAR(window_centre.y, 0.0, 1e-9) << "y = 1 is the centre of a [-1, 3] window";
    // ...and the bounds map to the NDC edges, so the window really is the projected frustum.
    const auto left_edge = cam.projectionMatrix() * vn::math::Point3d(-2.0, -1.0, -0.5);
    EXPECT_NEAR(left_edge.x, -1.0, 1e-9);
    EXPECT_NEAR(left_edge.y, -1.0, 1e-9);
    const auto right_edge = cam.projectionMatrix() * vn::math::Point3d(6.0, 3.0, -0.5);
    EXPECT_NEAR(right_edge.x, 1.0, 1e-9);
    EXPECT_NEAR(right_edge.y, 1.0, 1e-9);
}

TEST(CameraTest, TheOrthographicPickingRayTravelsAcrossTheWindow)
{
    // The same window drives picking: screen [0, 1] maps onto [left, right] x [bottom, top], so the
    // corner of the screen asks about the corner of the window (a centred ray would answer about a
    // frustum the camera does not use).
    Camera cam;
    cam.setViewMatrixAsLookAt(Vec3d(0, 0, 10), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam.setProjectionMatrixAsOrtho(-2.0, 6.0, -1.0, 3.0, 0.5, 100.0);

    const Ray centre = cam.screenToWorldRay(Vec2d(0.5, 0.5));
    EXPECT_NEAR(centre.origin.x, 2.0, 1e-6) << "the window's horizontal centre is x = (+2 + 6)/2";
    EXPECT_NEAR(centre.origin.y, 1.0, 1e-6) << "its vertical centre is y = (-1 + 3)/2";
    EXPECT_NEAR(centre.origin.z, 10.0, 1e-6);
    EXPECT_NEAR(centre.direction.z, -1.0, 1e-6);

    // Bottom-left of the screen: the window's bottom-left corner, and every ray parallel to the view
    // direction (that is what makes an orthographic projection orthographic).
    const Ray bottom_left = cam.screenToWorldRay(Vec2d(0.0, 1.0));
    EXPECT_NEAR(bottom_left.origin.x, -2.0, 1e-6);
    EXPECT_NEAR(bottom_left.origin.y, -1.0, 1e-6);
    EXPECT_NEAR(bottom_left.direction.z, -1.0, 1e-6);

    const Ray top_right = cam.screenToWorldRay(Vec2d(1.0, 0.0));
    EXPECT_NEAR(top_right.origin.x, 6.0, 1e-6);
    EXPECT_NEAR(top_right.origin.y, 3.0, 1e-6);
}

TEST(CameraTest, ThePickingRayIsBuiltFromTheViewMatrixBasis)
{
    // The ray used to rebuild its basis from eye/target/up with a bare cross product, which divides by a
    // length near zero when the up vector is parallel to the view direction: lookAt() resolves that case
    // by picking a reference axis, so the picture was fine while picking returned NaNs. Building the ray
    // from the view matrix's own rows makes the two the same computation, degenerate case included.
    Camera cam;
    // Up parallel to the view direction (looking down -Y with up = +Y).
    cam.setViewMatrixAsLookAt(Vec3d(0, 10, 0), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam.setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    const Ray ray = cam.screenToWorldRay(Vec2d(0.5, 0.5));
    EXPECT_TRUE(std::isfinite(ray.direction.x));
    EXPECT_TRUE(std::isfinite(ray.direction.y));
    EXPECT_TRUE(std::isfinite(ray.direction.z));
    EXPECT_NEAR(ray.direction.length(), 1.0, 1e-9);
    // The centre ray is the view direction, exactly as the view matrix defines it.
    EXPECT_NEAR(ray.direction.y, -1.0, 1e-6);
    EXPECT_NEAR(ray.origin.y, 10.0, 1e-6);

    // A screen point off the centre turns towards the VIEW MATRIX's right/up rows: the ray has to be
    // built from the basis the picture uses, which is the only basis the degenerate case leaves intact.
    const Mat4d view = cam.viewMatrix();
    const Vec3d right(view.element(0, 0), view.element(0, 1), view.element(0, 2));
    const Vec3d up(view.element(1, 0), view.element(1, 1), view.element(1, 2));
    const Ray   off_centre = cam.screenToWorldRay(Vec2d(0.9, 0.1));
    EXPECT_TRUE(std::isfinite(off_centre.direction.x));
    EXPECT_TRUE(std::isfinite(off_centre.direction.y));
    EXPECT_TRUE(std::isfinite(off_centre.direction.z));
    EXPECT_NEAR(off_centre.direction.length(), 1.0, 1e-9);
    EXPECT_GT(off_centre.direction.dot(right), 0.0) << "a point right of centre must aim towards +right";
    EXPECT_GT(off_centre.direction.dot(up), 0.0) << "and a point above centre towards +up";
    // Perspective keeps the eye as the origin for every ray; only the direction turns.
    EXPECT_NEAR(off_centre.origin.y, 10.0, 1e-6);
}

TEST(CameraTest, TheProjectionIsBuiltForTheCamerasOwnShape)
{
    // ONE CAMERA, ONE PICTURE SHAPE (see the class note): the shape is state the camera holds, so the same
    // camera answers ONE projection - and a host that wants the same eye at another shape states it (or uses
    // another camera). Only the HORIZONTAL scale follows the shape: the vertical fov is the camera's own
    // parameter.
    Camera cam;
    cam.setProjectionMatrixAsPerspective(60.0, 0.1, 100.0);

    EXPECT_DOUBLE_EQ(cam.aspectRatio(), 1.0) << "a camera is born square, which is what a headless picture is";
    const Mat4d square = cam.projectionMatrix();

    cam.setAspect(2.0);
    EXPECT_DOUBLE_EQ(cam.aspectRatio(), 2.0);
    const Mat4d wide = cam.projectionMatrix();
    EXPECT_NEAR(wide.element(0, 0), square.element(0, 0) / 2.0, 1e-9)
        << "twice as wide a picture halves the world's x scale";
    EXPECT_NEAR(wide.element(1, 1), square.element(1, 1), 1e-9) << "the vertical fov belongs to the camera";

    // A non-positive shape is ignored rather than stored: NaN would poison every projection built from it.
    cam.setAspect(0.0);
    cam.setAspect(-3.0);
    EXPECT_DOUBLE_EQ(cam.aspectRatio(), 2.0) << "the last GOOD shape stays in force";

    // An ORTHOGRAPHIC camera is not fitted to a shape at all: its window already IS one (the shadow-map and the
    // tiled-window case), so the aspect is not folded in.
    Camera ortho;
    ortho.setProjectionMatrixAsOrtho(-2.0, 2.0, -1.0, 1.0, 0.1, 100.0);
    const Mat4d square_window = ortho.projectionMatrix();
    ortho.setAspect(16.0 / 9.0);
    EXPECT_TRUE(square_window.isEqual(ortho.projectionMatrix(), 1e-12))
        << "an orthographic window carries its own shape, whatever the camera's aspect says";
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
    cam->setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vn::window::ResizeEvent{ 800, 600 });

    Vec3d hit;
    ASSERT_TRUE(manip.pickAt(400.0, 300.0, hit));
    EXPECT_NEAR(hit.z, 0.0, 1e-6);  // on the triangle plane (in front of target)

    vn::window::MouseEvent press;
    press.button = vn::window::MouseButton::Left;
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
    cam->setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vn::window::ResizeEvent{ 800, 600 });

    Vec3d hit;
    EXPECT_FALSE(manip.pickAt(400.0, 300.0, hit));

    vn::window::MouseEvent press;
    press.button = vn::window::MouseButton::Left;
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
    const vn::math::Point3d box_c = scene.boundingBox().center();
    // The rotate pivot for a press on empty space is the scene centre.
    const Vec3d anchor(box_c.x, box_c.y, box_c.z);

    auto cam = intrusive_ptr<Camera>(new Camera());
    // Look at the origin; the triangle sits off to the right, so the centre
    // pixel ray misses it (the rotate press happens on empty space).
    const Vec3d eye(0, 0, 5);
    cam->setViewMatrixAsLookAt(eye, Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam->setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vn::window::ResizeEvent{ 800, 600 });

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
        const double tan_half = std::tan(cam->fieldOfView() * vn::math::DEG_TO_RAD * 0.5);
        // The picture's shape is the viewport's - the manipulator is told this same 800x600 by onResize below,
        // and it derives its own aspect from it (Camera holds none), so the helper reads the shape from the one
        // rectangle both of them draw into.
        const double aspect = w / h;
        const double ndc_x = lx / (lz * tan_half * aspect);
        const double ndc_y = ly / (lz * tan_half);
        return std::make_pair((ndc_x * 0.5 + 0.5) * w, (0.5 - ndc_y * 0.5) * h);
    };

    vn::window::MouseEvent press;
    press.button = vn::window::MouseButton::Left;
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
        vn::window::MouseEvent move;
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
    cam->setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get());
    manip.onResize(vn::window::ResizeEvent{ 800, 600 });

    vn::window::MouseEvent press;
    press.button = vn::window::MouseButton::Left;
    press.x = 400.0;
    press.y = 300.0;
    press.pressed = true;
    manip.onMousePress(press);

    const Vec3d world_up(0.0, 1.0, 0.0);

    vn::window::MouseEvent move;
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
    cam->setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vn::window::ResizeEvent{ 800, 600 });

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
    cam->setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    EXPECT_TRUE(manip.fitToScreen());
    EXPECT_NEAR(manip.orbitCenter().x, box.center().x, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().y, box.center().y, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().z, box.center().z, 1e-6);
    const double half_diag = (box.size() * 0.5).length();
    EXPECT_GT(manip.orbitRadius(), half_diag);
    EXPECT_NEAR((cam->eye() - cam->target()).length(), manip.orbitRadius(), 1e-6);
}

TEST(CameraManipulatorTest, TheInteractionShapeIsTheCameraPictureNotThePane)
{
    // ONE CAMERA, ONE PICTURE SHAPE (see Camera): the interaction works in the shape of the picture the user is
    // pointing at - gains, framing, and the orthographic window - and that shape is the camera's, the same number
    // the projection and the picking ray use. A manipulator that kept its own copy of the last resize would let
    // the two drift; what it keeps from a resize is the PIXEL extent (below), which is a different fact.
    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.1, 100.0);

    Scene                 scene;
    OrbitCameraManipulator manip(cam.get(), &scene);

    // A 4:3 pane, and a 2:1 camera: the window is the CAMERA's shape (the interaction would otherwise frame for a
    // picture nobody is looking at).
    cam->setAspect(2.0);
    manip.onResize(vn::window::ResizeEvent{ 800, 600 });
    EXPECT_DOUBLE_EQ(cam->orthographicRight() - cam->orthographicLeft(),
                     2.0 * (cam->orthographicTop() - cam->orthographicBottom()));

    // The camera is given another shape: the window follows it, and the pane is not consulted.
    cam->setAspect(3.0);
    manip.onResize(vn::window::ResizeEvent{ 800, 600 });
    EXPECT_DOUBLE_EQ(cam->orthographicRight() - cam->orthographicLeft(),
                     3.0 * (cam->orthographicTop() - cam->orthographicBottom()));
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
    auto geom = geometryOf(*makeUnitTriangle());

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
    auto geom = geometryOf(*makeUnitTriangle());

    // Ray aimed outside the triangle.
    Ray ray(Vec3d(5, 5, 1), Vec3d(0, 0, -1));
    RayIntersectionResult result = RayIntersection::intersect(ray, geom.get(), Mat4d());
    EXPECT_FALSE(result.hit);
}

TEST(RayIntersectionTest, SceneQuery)
{
    auto scene = intrusive_ptr<Scene>(new Scene());
    auto node = intrusive_ptr<Group>(new Group());
    auto geom = geometryOf(*makeUnitTriangle());
    geom->setName(u8"tri");
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
    auto mesh = intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, -2.0f),
                      vn::math::Vec3f(1.0f, 0.0f, -2.0f),
                      vn::math::Vec3f(0.0f, 1.0f, -2.0f));
    auto geom = geometryOf(*mesh);
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

TEST(RayIntersectionTest, ASingularWorldTransformIsSkipped)
{
    // Matrix4x4::inverted() cannot report failure (it returns the original matrix for a singular
    // one), so the picker used to build its local space from a matrix that is not the inverse at
    // all and answered from that wrong space. A world transform that collapses an axis has no
    // volume to intersect, so the only honest answer is "miss".
    auto geom = geometryOf(*makeUnitTriangle());
    auto root = intrusive_ptr<Group>(new Group());
    auto transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    transform->setMatrix(vn::math::scale(Vec3d(0.0, 1.0, 1.0))); // x collapsed
    transform->addChild(geom);
    root->addChild(transform);
    auto scene = intrusive_ptr<Scene>(new Scene());
    scene->setRoot(root);

    // The ray crosses the triangle's plane exactly on the collapsed axis, i.e. the AABB slab test
    // alone cannot reject it — the invertibility check is what has to.
    Ray ray(Vec3d(0.0, 0.25, 1.0), Vec3d(0, 0, -1));
    EXPECT_FALSE(RayIntersection::intersect(ray, geom.get(), transform->matrix()).hit);
    EXPECT_FALSE(RayIntersection::intersectScene(ray, scene.get()).hit);

    // Positive control: the same setup with a real (tiny) scale still hits, so the guard rejects
    // degeneracy rather than "everything that is not the identity".
    auto thin = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    thin->setMatrix(vn::math::scale(Vec3d(1e-4, 1.0, 1.0)));
    EXPECT_TRUE(RayIntersection::intersect(ray, geom.get(), thin->matrix()).hit);
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

    EXPECT_EQ(mat.name(), u8"red");
    EXPECT_NEAR(mat.diffuse().r, 1.0f, 1e-6f);
    EXPECT_NEAR(mat.shininess(), 64.0f, 1e-6f);
}

TEST(MaterialTest, CarriesTheTextureItSamples)
{
    auto texture = intrusive_ptr<Texture>(new Texture2D(4, 4, PixelFormat::Rgba8Unorm));

    Material mat;
    EXPECT_EQ(mat.texture(), nullptr);

    mat.setTexture(texture);
    EXPECT_EQ(mat.texture(), texture.get());
    EXPECT_EQ(texture->useCount(), 2u);

    mat.setTexture(nullptr);
    EXPECT_EQ(mat.texture(), nullptr);
}

// ============ Material inheritance ============

namespace
{

/// @brief Collects the commands of a scene rooted at @p root.
std::vector<RenderCommand> commandsOf(const intrusive_ptr<Node>& root, Camera& camera)
{
    Scene scene;
    scene.setRoot(root);
    return scene.collectRenderCommands(&camera);
}

} // namespace

TEST(MaterialInheritanceTest, ASubtreeMaterialCoversEveryGeometryUnderIt)
{
    // The point of the feature: one material on a subtree instead of one per Geometry.
    auto material = MaterialPtr(new Material());
    auto state    = intrusive_ptr<StateNode>(new StateNode());
    state->setMaterial(material);
    state->addChild(geometryOf(*makeUnitTriangle()));

    Camera     camera;
    setupLookAtCamera(camera);
    const auto commands = commandsOf(state, camera);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), material.get());
}

TEST(MaterialInheritanceTest, AGeometrysOwnMaterialWinsOverAnAncestor)
{
    auto inherited = MaterialPtr(new Material());
    auto own       = MaterialPtr(new Material());

    auto geometry = geometryOf(*makeUnitTriangle());
    geometry->setMaterial(own);

    auto state = intrusive_ptr<StateNode>(new StateNode());
    state->setMaterial(inherited);
    state->addChild(geometry);

    Camera     camera;
    setupLookAtCamera(camera);
    const auto commands = commandsOf(state, camera);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), own.get()) << "the leaf is consulted first";
}

TEST(MaterialInheritanceTest, TheNearestAncestorWinsWhenSeveralSetOne)
{
    auto outermost = MaterialPtr(new Material());
    auto innermost = MaterialPtr(new Material());

    auto inner = intrusive_ptr<StateNode>(new StateNode());
    inner->setMaterial(innermost);
    inner->addChild(geometryOf(*makeUnitTriangle()));

    auto outer = intrusive_ptr<StateNode>(new StateNode());
    outer->setMaterial(outermost);
    outer->addChild(inner);

    Camera     camera;
    setupLookAtCamera(camera);
    const auto commands = commandsOf(outer, camera);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), innermost.get()) << "a material replaces the shading wholesale";
}

TEST(MaterialInheritanceTest, ClearingTheOverrideRestoresInheritance)
{
    auto inherited = MaterialPtr(new Material());

    auto inner = intrusive_ptr<StateNode>(new StateNode());
    inner->setMaterial(MaterialPtr(new Material()));
    inner->addChild(geometryOf(*makeUnitTriangle()));

    auto outer = intrusive_ptr<StateNode>(new StateNode());
    outer->setMaterial(inherited);
    outer->addChild(inner);

    Camera camera;
    setupLookAtCamera(camera);

    inner->clearMaterial();
    const auto commands = commandsOf(outer, camera);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), inherited.get());
}

TEST(MaterialInheritanceTest, TwoSubtreesWithDifferentMaterialsResolveSeparately)
{
    // Resolution is per PATH, not a single "first one found": two branches of one tree keep their own.
    auto left_material  = MaterialPtr(new Material());
    auto right_material = MaterialPtr(new Material());

    auto left = intrusive_ptr<StateNode>(new StateNode());
    left->setMaterial(left_material);
    left->addChild(geometryOf(*makeUnitTriangle()));

    auto right = intrusive_ptr<StateNode>(new StateNode());
    right->setMaterial(right_material);
    right->addChild(geometryOf(*makeUnitTriangle()));

    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(left);
    root->addChild(right);

    Camera     camera;
    setupLookAtCamera(camera);
    const auto commands = commandsOf(root, camera);

    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].material.get(), left_material.get());
    EXPECT_EQ(commands[1].material.get(), right_material.get());
}

TEST(MaterialInheritanceTest, WithoutAnyMaterialTheCommandCarriesNone)
{
    // Null has to stay reachable: that is how "let the engine default apply" is expressed.
    auto state = intrusive_ptr<StateNode>(new StateNode());
    state->addChild(geometryOf(*makeUnitTriangle()));

    Camera     camera;
    setupLookAtCamera(camera);
    const auto commands = commandsOf(state, camera);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), nullptr);
}

// ============ Texture ============

TEST(TextureTest, A2DTextureIsOneFace)
{
    Texture2D texture(8, 4, PixelFormat::Rgba8Unorm, 2);

    EXPECT_EQ(texture.kind(), Texture::Kind::D2);
    EXPECT_EQ(texture.faceCount(), 1);
    EXPECT_EQ(texture.width(), 8);
    EXPECT_EQ(texture.height(), 4);
    EXPECT_EQ(texture.format(), PixelFormat::Rgba8Unorm);
    EXPECT_EQ(texture.mipCount(), 2);
    EXPECT_EQ(texture.source(0), nullptr);
    EXPECT_FALSE(texture.isComplete());
}

TEST(TextureTest, ACubeTextureIsSixFacesSharingOneDescription)
{
    CubeMap texture(16, PixelFormat::Rgba8Unorm);

    EXPECT_EQ(texture.faceCount(), 6);
    EXPECT_EQ(texture.width(), 16);
    EXPECT_EQ(texture.height(), 16);

    for (int face = 0; face < 6; ++face) {
        EXPECT_FALSE(texture.hasSource(face)) << "face " << face;
    }
}

TEST(TextureTest, BecomesCompleteOnlyWhenEveryFaceIsFilled)
{
    CubeMap texture(4, PixelFormat::Rgba8Unorm);

    for (int face = 0; face < 5; ++face) {
        texture.setSource(face, sourceImage(texture, 1));
        EXPECT_FALSE(texture.isComplete()) << "face " << face;
    }

    texture.setSource(5, sourceImage(texture, 1));
    EXPECT_TRUE(texture.isComplete());
}

TEST(TextureTest, HoldsItsFacesByStrongReference)
{
    CubeMap texture(4, PixelFormat::Rgba8Unorm);

    intrusive_ptr<const Image> face = sourceImage(texture, 1);
    texture.setSource(2, face);

    EXPECT_EQ(texture.source(2), face.get());
    EXPECT_EQ(face->useCount(), 2u);

    texture.setSource(2, nullptr);
    EXPECT_EQ(face->useCount(), 1u);
    EXPECT_FALSE(texture.hasSource(2));
}

TEST(TextureTest, AnOutOfRangeFaceIsRejectedWhenWritten)
{
    Texture2D d2(4, 4, PixelFormat::Rgba8Unorm);
    CubeMap cube(4, PixelFormat::Rgba8Unorm);

    EXPECT_THROW(d2.setSource(1, nullptr), std::out_of_range);
    EXPECT_THROW(d2.setSource(-1, nullptr), std::out_of_range);
    EXPECT_THROW(cube.setSource(6, nullptr), std::out_of_range);

    // Reading outside the shape is a query, not a mistake: it answers "no such face".
    EXPECT_EQ(d2.source(1), nullptr);
    EXPECT_EQ(cube.source(-1), nullptr);
    EXPECT_FALSE(d2.hasSource(1));
}

TEST(TextureTest, RejectsASourceThatDoesNotMatchTheDescription)
{
    Texture2D texture(8, 4, PixelFormat::Rgba8Unorm, 2);

    // Each of these differs from the description in exactly one field, so the check that fires is the one
    // under test rather than an earlier one.
    intrusive_ptr<const Image> wrong_format(new Image(8, 4, PixelFormat::Rgba32Float, 2));
    intrusive_ptr<const Image> wrong_size(new Image(4, 4, PixelFormat::Rgba8Unorm, 2));
    intrusive_ptr<const Image> wrong_mips(new Image(8, 4, PixelFormat::Rgba8Unorm, 1));

    EXPECT_THROW(texture.setSource(0, wrong_format), std::invalid_argument);
    EXPECT_THROW(texture.setSource(0, wrong_size), std::invalid_argument);
    EXPECT_THROW(texture.setSource(0, wrong_mips), std::invalid_argument);
    EXPECT_FALSE(texture.hasSource(0)) << "a rejected source must not be stored";

    texture.setSource(0, sourceImage(texture, 2));
    EXPECT_TRUE(texture.hasSource(0));
    EXPECT_TRUE(texture.isComplete());
}

TEST(TextureTest, RejectsAnImpossibleDescription)
{
    EXPECT_THROW(Texture2D(0, 4, PixelFormat::Rgba8Unorm), std::invalid_argument);
    EXPECT_THROW(Texture2D(4, 0, PixelFormat::Rgba8Unorm), std::invalid_argument);
    EXPECT_THROW(CubeMap(4, PixelFormat::Unknown), std::invalid_argument);

    // 4x4 holds exactly 3 levels, so 0 and 4 are both impossible.
    EXPECT_THROW(CubeMap(4, PixelFormat::Rgba8Unorm, 0), std::invalid_argument);
    EXPECT_THROW(CubeMap(4, PixelFormat::Rgba8Unorm, 4), std::invalid_argument);

    EXPECT_NO_THROW(CubeMap(4, PixelFormat::Rgba8Unorm, 3));
}

TEST(TextureTest, KindNamesItselfForDiagnostics)
{
    EXPECT_STREQ(Texture::kindName(Texture::Kind::D2), "D2");
    EXPECT_STREQ(Texture::kindName(Texture::Kind::Cube), "Cube");
    EXPECT_STREQ(Texture::kindName(static_cast<Texture::Kind>(99)), "Unknown");
}

TEST(TextureTest, FillingAFaceBumpsTheContentRevision)
{
    // A backend caches the uploaded image per texture ADDRESS, so a re-filled texture has to be
    // distinguishable from an unchanged one or it keeps sampling the old upload — the same failure the
    // shader program's revision exists to prevent.
    Texture2D texture(4, 4, PixelFormat::Rgba8Unorm);
    EXPECT_EQ(texture.revision(), 0u);

    texture.setSource(0, sourceImage(texture, 1));
    const std::uint64_t after_fill = texture.revision();
    EXPECT_GT(after_fill, 0u);

    // Reading is not a mutation.
    (void)texture.source(0);
    (void)texture.isComplete();
    EXPECT_EQ(texture.revision(), after_fill);

    // Replacing the pixels counts, as does clearing the face.
    texture.setSource(0, sourceImage(texture, 1));
    EXPECT_GT(texture.revision(), after_fill);

    const std::uint64_t before_clear = texture.revision();
    texture.setSource(0, nullptr);
    EXPECT_GT(texture.revision(), before_clear);
}

// ============ Texture2D / CubeMap ============

TEST(Texture2DTest, IsOneImageAndTakesNoFaceIndex)
{
    Texture2D texture(8, 4, PixelFormat::Rgba8Unorm, 2);

    EXPECT_EQ(texture.kind(), Texture::Kind::D2);
    EXPECT_EQ(texture.layerCount(), 1);
    EXPECT_EQ(texture.faceCount(), 1);
    EXPECT_EQ(texture.width(), 8);
    EXPECT_EQ(texture.height(), 4);
    EXPECT_EQ(texture.image(), nullptr);
    EXPECT_FALSE(texture.isComplete());

    texture.setImage(sourceImage(texture, 2));
    EXPECT_TRUE(texture.isComplete());
    EXPECT_EQ(texture.image(), texture.source(0));
    EXPECT_EQ(texture.layer(0), texture.image());
}

TEST(Texture2DTest, AnOutOfRangeLayerIsAbsent)
{
    Texture2D texture(4, 4, PixelFormat::Rgba8Unorm);

    // The shape-agnostic spelling follows the same rule as the numbered one: reading outside the shape is a
    // query with the answer "no such layer", not an error.
    EXPECT_EQ(texture.layer(1), nullptr);
    EXPECT_EQ(texture.layer(-1), nullptr);
}

TEST(Texture2DTest, RejectsAnImageThatDoesNotMatchTheDescription)
{
    Texture2D texture(8, 4, PixelFormat::Rgba8Unorm, 2);

    intrusive_ptr<const Image> wrong_size(new Image(4, 4, PixelFormat::Rgba8Unorm, 2));
    EXPECT_THROW(texture.setImage(wrong_size), std::invalid_argument);
    EXPECT_EQ(texture.image(), nullptr) << "a rejected image must not be stored";

    texture.setImage(sourceImage(texture, 2));
    EXPECT_EQ(texture.image()->width(), 8);
}

TEST(CubeMapTest, IsSixSquareFacesOfOneSize)
{
    // The faces are square BY CONSTRUCTION: a cube whose faces were not square is a description no sampling
    // hardware accepts, so the size is stated once and cannot disagree with itself.
    CubeMap cube(16, PixelFormat::Rgba8Unorm, 3);

    EXPECT_EQ(cube.kind(), Texture::Kind::Cube);
    EXPECT_EQ(cube.layerCount(), 6);
    EXPECT_EQ(cube.width(), 16);
    EXPECT_EQ(cube.height(), 16);
    EXPECT_EQ(cube.mipCount(), 3);
    EXPECT_FALSE(cube.isComplete());
}

TEST(CubeMapTest, FacesAreNamedInTheLayerOrderVulkanReads)
{
    // The numbering is not a free choice: Vulkan fixes the layer order, so a named face has to land on the
    // layer a sampler will read it from — otherwise the cube samples the wrong sides and nothing else in the
    // pipeline would notice.
    EXPECT_EQ(static_cast<int>(CubeMap::Face::PosX), 0);
    EXPECT_EQ(static_cast<int>(CubeMap::Face::NegX), 1);
    EXPECT_EQ(static_cast<int>(CubeMap::Face::PosY), 2);
    EXPECT_EQ(static_cast<int>(CubeMap::Face::NegY), 3);
    EXPECT_EQ(static_cast<int>(CubeMap::Face::PosZ), 4);
    EXPECT_EQ(static_cast<int>(CubeMap::Face::NegZ), 5);

    EXPECT_STREQ(CubeMap::faceName(CubeMap::Face::PosX), "+X");
    EXPECT_STREQ(CubeMap::faceName(CubeMap::Face::NegZ), "-Z");
    EXPECT_STREQ(CubeMap::faceName(static_cast<CubeMap::Face>(99)), "Unknown");
}

TEST(CubeMapTest, EachFaceIsFilledAndReadBackByName)
{
    CubeMap cube(4, PixelFormat::Rgba8Unorm);

    intrusive_ptr<const Image> front = sourceImage(cube, 1);
    cube.setFaceImage(CubeMap::Face::PosZ, front);

    EXPECT_EQ(cube.faceImage(CubeMap::Face::PosZ), front.get());
    EXPECT_EQ(cube.faceImage(CubeMap::Face::NegZ), nullptr);
    EXPECT_EQ(cube.layer(4), front.get()) << "faceImage(PosZ) must be the layer Vulkan reads as +Z";
    EXPECT_FALSE(cube.isComplete());

    for (const CubeMap::Face face : { CubeMap::Face::PosX, CubeMap::Face::NegX, CubeMap::Face::PosY,
                                      CubeMap::Face::NegY, CubeMap::Face::NegZ }) {
        cube.setFaceImage(face, sourceImage(cube, 1));
    }
    EXPECT_TRUE(cube.isComplete());
}

TEST(CubeMapTest, FillingAFaceBumpsTheContentRevision)
{
    CubeMap cube(4, PixelFormat::Rgba8Unorm);
    const std::uint64_t before = cube.revision();

    cube.setFaceImage(CubeMap::Face::NegY, sourceImage(cube, 1));
    EXPECT_GT(cube.revision(), before);
}

TEST(TextureLayersTest, BothSpellingsDescribeTheSameTexture)
{
    // `layer()`/`layerCount()` is what a backend walks and `source()`/`faceCount()` is what the numbered API
    // exposes. They have to be the same view of a texture, or a consumer that moved to the generic spelling
    // would upload something different from one that did not.
    Texture2D d2(4, 4, PixelFormat::Rgba8Unorm);
    d2.setImage(sourceImage(d2, 1));

    CubeMap cube(4, PixelFormat::Rgba8Unorm);
    for (int face = 0; face < 6; ++face) {
        cube.setSource(face, sourceImage(cube, 1));
    }

    const Texture* shapes[2] = { &d2, &cube };
    for (const Texture* texture : shapes) {
        EXPECT_EQ(texture->layerCount(), texture->faceCount()) << Texture::kindName(texture->kind());
        for (int index = 0; index < texture->layerCount(); ++index) {
            EXPECT_EQ(texture->layer(index), texture->source(index)) << "layer " << index;
        }
        EXPECT_EQ(texture->layer(texture->layerCount()), nullptr);
    }

    EXPECT_EQ(d2.layerCount(), 1);
    EXPECT_EQ(cube.layerCount(), 6);
}

// ============ Geometry ============

TEST(GeometryTest, MeshCounts)
{
    auto geom = geometryOf(*makeUnitTriangle());

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

TEST(NodeTest, ChildHierarchyReparentsInsteadOfAdoptingTwice)
{
    auto first = intrusive_ptr<Group>(new Group());
    auto second = intrusive_ptr<Group>(new Group());
    auto child = intrusive_ptr<Node>(new Node());

    first->addChild(child);
    second->addChild(child);

    // The child moved: a node has exactly one parent, which is the invariant
    // the per-traversal world matrix / bounds cache relies on.
    EXPECT_EQ(first->children().size(), 0u);
    EXPECT_EQ(second->children().size(), 1u);
    EXPECT_EQ(child->parent(), second.get());
}

TEST(NodeTest, AddChildRejectsCycles)
{
    // A cyclic graph would make every tree traversal (command collection,
    // bounds, picking, find) recurse forever, so the add must be rejected and
    // leave the graph untouched.
    auto outer = intrusive_ptr<Group>(new Group());
    auto middle = intrusive_ptr<Group>(new Group());
    auto leaf = intrusive_ptr<Group>(new Group());
    outer->addChild(middle);
    middle->addChild(leaf);

    // Self-parenting is rejected.
    outer->addChild(outer);
    EXPECT_EQ(outer->parent(), nullptr);
    EXPECT_EQ(outer->children().size(), 1u);

    // Direct cycle: making an ancestor a child of its own descendant.
    leaf->addChild(outer);
    EXPECT_EQ(leaf->children().size(), 0u);
    EXPECT_EQ(outer->parent(), nullptr);
    EXPECT_EQ(outer->children().size(), 1u);

    // Indirect cycle, one level up: middle adopting outer.
    middle->addChild(outer);
    EXPECT_EQ(middle->children().size(), 1u);   // still just `leaf`
    EXPECT_EQ(middle->childrenRef()[0].get(), leaf.get());
    EXPECT_EQ(outer->childrenRef()[0].get(), middle.get());

    // The tree is still traversable (no infinite recursion) and its structure
    // is exactly the one built before the rejected adds.
    Scene scene;
    scene.setRoot(outer);
    auto geom = geometryOf(*makeUnitTriangle());
    leaf->addChild(geom);
    Camera cam;
    setupLookAtCamera(cam);
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
}

TEST(NodeTest, WorldTransformCascades)
{
    auto parent = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto child = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    parent->addChild(child);

    // Parent translates by (1, 0, 0), child by (2, 0, 0).
    parent->setMatrix(vn::math::translate(Vec3d(1, 0, 0)));
    child->setMatrix(vn::math::translate(Vec3d(2, 0, 0)));

    // Child world origin = parent translation * child translation = (3, 0, 0).
    const auto origin = child->worldMatrix() * vn::math::Point3d(0, 0, 0);
    EXPECT_NEAR(origin.x, 3.0, 1e-9);
    EXPECT_NEAR(origin.y, 0.0, 1e-9);
    EXPECT_NEAR(origin.z, 0.0, 1e-9);
}

TEST(NodeTest, BoundingBoxWithTransform)
{
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto geom = geometryOf(*makeUnitTriangle());
    node->addChild(geom);
    node->setMatrix(vn::math::translate(Vec3d(10, 0, 0)));

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

    const Mat4d m = vn::math::translate(Vec3d(1, 2, 3));
    mt.setMatrix(m);
    EXPECT_NEAR(mt.matrix().element(0, 3), 1.0, 1e-9);
    EXPECT_NEAR(mt.matrix().element(1, 3), 2.0, 1e-9);
    EXPECT_NEAR(mt.matrix().element(2, 3), 3.0, 1e-9);
}

TEST(MatrixTransformTest, WorldMatrixAccumulatesNested)
{
    auto outer = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    outer->setMatrix(vn::math::translate(Vec3d(1, 0, 0)));
    auto inner = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    inner->setMatrix(vn::math::translate(Vec3d(0, 2, 0)));
    outer->addChild(inner);

    // Outer translation then inner translation: (1, 2, 0).
    const auto p = inner->worldMatrix() * vn::math::Point3d(0, 0, 0);
    EXPECT_NEAR(p.x, 1.0, 1e-9);
    EXPECT_NEAR(p.y, 2.0, 1e-9);
    EXPECT_NEAR(p.z, 0.0, 1e-9);
}

TEST(MatrixTransformTest, PlainGroupPassesTransformThrough)
{
    auto root = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    root->setMatrix(vn::math::translate(Vec3d(1, 0, 0)));
    auto group = intrusive_ptr<Group>(new Group());
    auto leaf = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    leaf->setMatrix(vn::math::translate(Vec3d(0, 0, 3)));
    root->addChild(group);
    group->addChild(leaf);

    // A plain Group contributes no matrix of its own.
    const auto p = leaf->worldMatrix() * vn::math::Point3d(0, 0, 0);
    EXPECT_NEAR(p.x, 1.0, 1e-9);
    EXPECT_NEAR(p.y, 0.0, 1e-9);
    EXPECT_NEAR(p.z, 3.0, 1e-9);
}

TEST(MatrixTransformTest, NestedBoundingBoxIsWorld)
{
    auto outer = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    outer->setMatrix(vn::math::translate(Vec3d(10, 0, 0)));
    auto inner = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    inner->setMatrix(vn::math::translate(Vec3d(0, 5, 0)));
    outer->addChild(inner);

    auto geom = geometryOf(*makeUnitTriangle());
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

TEST(NodeTest, TheLocalBoxPlacedByTheWorldMatrixIsTheWorldBox)
{
    // ONE RULE, TWO SPELLINGS. The collection walk transforms each leaf's LOCAL box by the matrix IT
    // accumulated (see Scene.cpp's bounds cache); the leaf's own boundingBox() transforms the same local
    // box by its own worldMatrix(). On a chain with rotation, translation and non-uniform scale the two
    // must be the SAME box - if they could disagree, culling would decide differently depending on which
    // spelling asked.
    auto outer = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    outer->setMatrix(vn::math::translate(Vec3d(3.0, -2.0, 1.0)) *
                     vn::math::rotate(Vec3d(0.0, 0.0, 1.0), 0.7) * vn::math::scale(Vec3d(2.0, 0.5, 1.5)));
    auto inner = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    inner->setMatrix(vn::math::rotate(Vec3d(1.0, 0.0, 0.0), 0.4) * vn::math::translate(Vec3d(0.0, 1.0, 0.0)));
    auto geom = geometryOf(*makeUnitTriangle());
    outer->addChild(inner);
    inner->addChild(geom);

    const Aabbd local = geom->localBounds();
    EXPECT_TRUE(local.isValid()) << "a triangle with positions has a local box";

    const Aabbd via_leaf = geom->boundingBox();
    const Aabbd via_walk = transformBox(geom->localBounds(), geom->worldMatrix());
    EXPECT_TRUE(via_leaf == via_walk) << "the walk's spelling and the leaf's spelling must be one box";

    // The local box belongs to the DATA, not to the placement: moving the chain leaves it alone while
    // the world box follows.
    const Aabbd local_before = geom->localBounds();
    outer->setMatrix(vn::math::translate(Vec3d(-5.0, 4.0, 2.0)));
    EXPECT_TRUE(geom->localBounds() == local_before) << "placement is not data";
    EXPECT_FALSE(geom->boundingBox() == via_leaf) << "while the world box follows the placement";
}

TEST(MatrixTransformTest, WorldMatrixIsOwnMatrixForSingleNode)
{
    // A lone MatrixTransform's worldMatrix is exactly its own matrix (no
    // self-doubling when folding the parent chain).
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    node->setMatrix(vn::math::translate(Vec3d(4, 0, 0)));
    const auto p = node->worldMatrix() * vn::math::Point3d(0, 0, 0);
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
    root->setMatrix(vn::math::translate(Vec3d(0, 0, -5)));
    auto holder = makeTriangleNode(Vec3d(0, 0, 0), nullptr, u8"tri");
    root->addChild(holder);
    scene.setRoot(root);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    // The command's model matrix is the folded world transform of the leaf.
    const auto origin = commands[0].modelMatrix * vn::math::Point3d(0, 0, 0);
    EXPECT_NEAR(origin.x, 0.0, 1e-9);
    EXPECT_NEAR(origin.y, 0.0, 1e-9);
    EXPECT_NEAR(origin.z, -5.0, 1e-9);
}

TEST(GeometryTest, CountsStayDataStatsUnderPointsTopology)
{
    // Topology lives on StateNode render state, NOT on the geometry data:
    // the vertex count is a pure data statistic and must not change when an
    // enclosing StateNode overrides the draw topology.
    auto geom = geometryOf(*makeUnitTriangle());  // 3 vertices
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
    // The draw topology rides the content's state (and the pass drawing it decides the rest - see
    // resolveRenderState); the data counts stay.
    ASSERT_TRUE(commands[0].authoredState.topology.has_value());
    EXPECT_EQ(*commands[0].authoredState.topology, Topology::Points);
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

TEST(GeometryTest, BumpRevisionMovesForwardFromWhereverItIs)
{
    Geometry geometry;
    EXPECT_EQ(geometry.revision(), 0u);

    // One call reports one edit, whatever the counter currently holds — a host that does not keep the
    // number itself (the whole reason setRevision(revision() + 1) was written by hand at every call site).
    geometry.bumpRevision();
    EXPECT_EQ(geometry.revision(), 1u);
    geometry.setRevision(41u);
    geometry.bumpRevision();
    EXPECT_EQ(geometry.revision(), 42u);

    // And the counter cannot be LOWERED by arithmetic: the hazard setRevision documents as "treat it as
    // monotonic" is not something a bumper can get wrong.
    for (int i = 0; i < 3; ++i) {
        geometry.bumpRevision();
    }
    EXPECT_EQ(geometry.revision(), 45u);
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
    mesh->setPositions({ vn::math::Vec3f(9.0f, 9.0f, 9.0f) });
    EXPECT_TRUE(mesh->aabb() == custom);
}

TEST(MeshTest, AttributesSharedOnBase)
{
    auto mesh = makeUnitTriangle();
    EXPECT_EQ(mesh->vertexCount(), 3u);
    EXPECT_EQ(mesh->positions().size(), 3u);
    EXPECT_EQ(mesh->triangleCount(), 1u);

    // Position/normal/texcoord storage lives on the Mesh base.
    mesh->setPositions({ vn::math::Vec3f(0, 0, 0), vn::math::Vec3f(2, 0, 0),
                         vn::math::Vec3f(0, 2, 0) });
    EXPECT_EQ(mesh->vertexCount(), 3u);
    EXPECT_EQ(mesh->positions()[1].x, 2.0f);

    mesh->setNormals({ vn::math::Vec3f(0, 0, 1), vn::math::Vec3f(0, 0, 1),
                      vn::math::Vec3f(0, 0, 1) });
    EXPECT_EQ(mesh->normals().size(), 3u);
}

TEST(MeshTest, AttributeStorageIsSharedNotCopied)
{
    intrusive_ptr<vn::geometry::IndexedTriangleMesh> mesh(new vn::geometry::IndexedTriangleMesh());
    mesh->addVertex(vn::math::Vec3f(0.0f, 0.0f, 0.0f));
    mesh->addVertex(vn::math::Vec3f(1.0f, 0.0f, 0.0f));

    // A second consumer (the renderer, here just a holder) takes the shareable handle rather than a copy.
    const auto shared = mesh->positionsBuffer();
    EXPECT_EQ(shared->size(), 2u * vn::geometry::Mesh::kVec3Components);
    // One allocation, not two: the typed mesh view and the scalar run the holder reads are the same bytes.
    EXPECT_EQ(shared->data(), reinterpret_cast<const float*>(mesh->positions().data()));
    EXPECT_EQ(shared->bytes().size(), 2u * sizeof(vn::math::Vec3f));

    // The model keeps building. The holder sees it because it IS the same storage, and the growth is
    // announced so a cache knows its copy of the bytes is stale.
    const auto before = shared->revision();
    mesh->addVertex(vn::math::Vec3f(2.0f, 0.0f, 0.0f));
    EXPECT_EQ(shared->size(), 3u * vn::geometry::Mesh::kVec3Components);
    EXPECT_EQ(shared->data(), reinterpret_cast<const float*>(mesh->positions().data()));
    EXPECT_GT(shared->revision(), before);

    // The scalars really are the vertices, in order.
    EXPECT_FLOAT_EQ(mesh->positions()[2].x, 2.0f);

    // The index buffer is shared the same way, and appending a triangle announces itself too.
    mesh->addTriangle(0u, 1u, 2u);
    const auto indices = mesh->indicesBuffer();
    EXPECT_EQ(indices->size(), 3u);
    EXPECT_EQ(indices->data(), mesh->indices().data());
}

TEST(MeshTest, EveryEditAnnouncesItselfOncePerEdit)
{
    // The announcement is per EDIT, not per element — which is exactly what only the writer can know, and the
    // reason `Buffer` does not move its own revision (an append used to bump once per scalar pushed). A holder
    // wants to hear "I am done", so the count says that: one per appended vertex, one per appended triangle,
    // one per cleared array.
    intrusive_ptr<vn::geometry::IndexedTriangleMesh> mesh(new vn::geometry::IndexedTriangleMesh());
    const auto positions = mesh->positionsBuffer();
    const auto indices   = mesh->indicesBuffer();
    ASSERT_NE(positions, nullptr);
    ASSERT_NE(indices, nullptr);
    EXPECT_EQ(positions->revision(), 0u);
    EXPECT_EQ(indices->revision(), 0u);

    mesh->addVertex(vn::math::Vec3f(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(positions->revision(), 1u) << "one vertex appended = one announcement";
    mesh->addVertex(vn::math::Vec3f(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(positions->revision(), 2u);

    mesh->addTriangle(0u, 1u, 1u);
    EXPECT_EQ(indices->revision(), 1u) << "one triangle appended = one announcement";

    mesh->clear();
    EXPECT_EQ(positions->revision(), 3u) << "emptying the attribute storage is an edit too";
    EXPECT_EQ(indices->revision(), 2u);
}

TEST(GeometryTest, BoundingBoxComputedFromBuffers)
{
    // The converter fills the geometry's buffers; the box derives from them.
    auto geom = geometryOf(*makeUnitTriangle());
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
    EXPECT_FALSE(rt.isValid());
    EXPECT_FALSE(rt.hasColor());
    EXPECT_FALSE(rt.hasDepth());

    rt.setSize(320, 240);
    rt.attachColor(RenderTarget::ColorFormat::RGBA8);
    rt.attachDepth(RenderTarget::DepthFormat::D24);

    EXPECT_TRUE(rt.hasColor());
    EXPECT_TRUE(rt.hasDepth());
    EXPECT_EQ(rt.colorFormat(), RenderTarget::ColorFormat::RGBA8);
    EXPECT_EQ(rt.depthFormat(), RenderTarget::DepthFormat::D24);
    EXPECT_TRUE(rt.isValid());
    EXPECT_EQ(rt.width(), 320);
    EXPECT_EQ(rt.height(), 240);

    // Zero-size target is never usable, even with attachments attached.
    rt.setSize(0, 0);
    EXPECT_FALSE(rt.isValid());
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
    EXPECT_TRUE(rt.isValid());
}

// ============ RenderEngine ============

/**
 * @brief Recording backend that counts calls for verification.
 */
class MockBackend : public RenderBackend {
  public:
    /// The WINDOW HALF of this double (see RenderDevice::createSurfaceTarget): what the engine forwarded to the
    /// target (a window announcement, a size) and the presents it drove. Frame cases count the present through it,
    /// and a double that wants no window half at all flips `has_surface` below.
    class MockSurface : public WindowPresentTarget
    {
      public:
        void setWindowHandle(void* handle) override
        {
            adopted_handle = handle;
            handle_sets.push_back(handle);
        }
        [[nodiscard]] void* nativeHandle() const override { return adopted_handle; }
        void resize(int width, int height) override
        {
            ++resize_calls;
            last_surface_width  = width;
            last_surface_height = height;
        }
        void present() override { ++present_calls; }

      public:
        int                present_calls       = 0;
        int                resize_calls        = 0;
        int                last_surface_width  = 0;
        int                last_surface_height = 0;
        void*              adopted_handle      = nullptr;
        std::vector<void*> handle_sets;
    };

    bool ok = false;

    /// Emits one diagnostic through the backend contract (test hook for the
    /// engine's sink pass-through; reportDiagnostic is protected).
    void emitDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category,
                        const vn::String& message)
    {
        reportDiagnostic(severity, category, message);
    }

    /// Whether this double hands a window half out at all (see RenderDevice::createSurfaceTarget): TRUE by
    /// default (a frame test is about a device that presents through a window, and the present is one of the
    /// witnesses below). A double that wants none flips `has_surface`.
    bool        has_surface = true;
    MockSurface surface_half;
    /// How many targets were asked for (see createSurfaceTarget), and the one this double handed out.
    int         target_creations = 0;
    [[nodiscard]] vn::raw_ptr<WindowPresentTarget> createSurfaceTarget(void* native_handle,
                                                                      SurfaceTargetResult* why = nullptr) override
    {
        ++target_creations;
        if (!has_surface) {
            if (why != nullptr) {
                *why = SurfaceTargetResult::NoPresentation;   // a device that presents nothing answers silently.
            }
            return nullptr;
        }
        surface_half.setWindowHandle(native_handle);
        if (why != nullptr) {
            *why = SurfaceTargetResult::Ok;
        }
        return &surface_half;
    }
    /// The frame half of this double is the double itself (see RenderDevice::executor).
    [[nodiscard]] RenderFrameExecutor& executor() override { return *this; }

    int render_calls = 0;
    int clear_calls = 0;
    /// Policy of the last clear the frame stated, so a test can assert WHAT the pass asked for.
    ClearPolicy last_clear_policy{};
    int viewport_sets = 0;
    const Camera* last_camera = nullptr;
    /// Camera of every content draw the frame carried, in run order (a pass' own camera, not just the last one).
    std::vector<const Camera*> render_cameras;
    /// How many commands every content draw carried, in run order (next to render_cameras): after a frame
    /// that ran several passes this says which pass drew what.
    std::vector<std::size_t> render_command_counts;
    int last_viewport[4] = { 0, 0, 0, 0 };
    // Programs of the commands of the last content draw (per command).
    std::vector<const ShaderProgram*> last_programs;
    // Rasterisation mode of the commands of the last content draw (per command).
    std::vector<PolygonMode> last_polygon_modes;
    int light_sets = 0;
    std::size_t last_light_count = 0;
    const Light* last_light = nullptr;
    int default_content_program_sets = 0;
    intrusive_ptr<const ShaderProgram> last_default_content_program;

    bool initialize() override { ok = true; return true; }
    void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program) override
    {
        last_default_content_program = std::move(program);
        ++default_content_program_sets;
    }
    void shutdown() override { ok = false; }
    void setRenderTarget(RenderTarget* target) override { target_history.push_back(target); }
    std::vector<RenderTarget*> target_history;
    int                        pass_input_sets = 0;
    std::vector<RenderTarget*> last_pass_inputs;

    /// The pass' own state, as the frame states it (see PassDescription::state).
    vn::graphics::PassRenderState pass_state{};
    /// What each command of the last content draw AUTHORS for its rasterisation mode (unset = "the content said
    /// nothing"): next to last_polygon_modes, which is what the pass state makes of it.
    std::vector<std::optional<vn::graphics::PolygonMode>> last_commands_authored_polygon_modes;
    int program_draws = 0;
    RenderTarget* last_program_source = nullptr;
    const ShaderProgram* last_program = nullptr;
    const Camera* last_program_camera = nullptr;
    /// What this double reports for RenderDevice::capabilities: off-screen targets TRUE by default, because
    /// the double honours every target it is handed (a pass test is not asking about target support).
    /// A test that wants the other answer flips this and the engine reports the pipeline it cannot serve.
    bool supports_targets = true;
    [[nodiscard]] Capabilities capabilities() const noexcept override
    {
        return Capabilities{ .render_targets = supports_targets };
    }

    /// The pass identities the frame named, in RUN ORDER (see FrameDescription::Pass::pass): what the engine's
    /// compile decided, as the backend sees it. `entries` counts the passes walked, and `entries_closed` how many
    /// of them the walk finished - equal once a frame is over, which is what makes "every entry is consumed" (and
    /// not silently dropped) checkable.
    std::vector<const RenderPass*> pass_entries;
    int entries = 0;
    int entries_closed = 0;
    /// How many pass entries carried an order, and the order of every entry, in run order.
    int order_sets = 0;
    std::vector<int> orders;

    /// How many frames were handed over (one executeFrame per frame the engine ran, see RenderFrameExecutor),
    /// and what each of them named as the target its default framebuffer resolves to (the area's target, or
    /// nullptr for a frame that presents nowhere).
    int frames_executed = 0;
    std::vector<PresentTarget*> present_targets;

    /// THE FRAME, WALKED BY THIS DOUBLE (see RenderFrameExecutor::executeFrame): one entry per pass, in run order -
    /// the identity, the order, the resolved inputs, then everything the pass issues (its target, the viewport /
    /// clear / state it stated, and each of its draws with the lights that belong to it). Every witness below is
    /// filled from HERE, which is what makes the other cases' assertions ("the backend was handed this clear",
    /// "this camera drew") read as "the engine's frame value said so".
    void executeFrame(const FrameDescription& frame, PresentTarget* present_to = nullptr) override
    {
        ++frames_executed;
        present_targets.push_back(present_to);
        for (const FrameDescription::Pass& entry : frame.passes) {
            if (entry.pass == nullptr) {
                continue;   // a frame's entry always names a pass; a null one is nothing to run
            }
            ++entries;
            pass_entries.push_back(entry.pass);
            ++order_sets;
            orders.push_back(entry.order);
            ++pass_input_sets;
            last_pass_inputs.assign(entry.inputs.begin(), entry.inputs.end());
            if (entry.description.has_value()) {
                consume(*entry.description);
            }
            ++entries_closed;
        }
    }

  private:
    /// @brief Records one pass' own facts and its drawing calls (the inside of the walk above).
    void consume(const PassDescription& description)
    {
        target_history.push_back(description.target);
        if (description.viewport.has_value()) {
            ++viewport_sets;
            last_viewport[0] = description.viewport->x;
            last_viewport[1] = description.viewport->y;
            last_viewport[2] = description.viewport->width;
            last_viewport[3] = description.viewport->height;
        }
        if (description.clear.has_value()) {
            ++clear_calls;
            last_clear_policy = *description.clear;
        }
        pass_state = description.state;
        for (const DrawDescription& draw : description.draws) {
            if (draw.lights.has_value()) {
                ++light_sets;
                last_light_count = draw.lights->size();
                last_light = draw.lights->empty() ? nullptr : (*draw.lights).front();
            }
            if (draw.camera == nullptr) {
                continue;   // a drawing call with no view is no call at all
            }
            if (draw.kind == DrawDescription::Kind::Screen) {
                ++program_draws;
                last_program_source = draw.source;
                last_program = draw.program.get();
                last_program_camera = draw.camera;
            }
            else if (draw.commands != nullptr) {
                consumeCommands(*draw.commands, draw.camera);
            }
        }
    }

    /// @brief Records one content drawing call's commands (the scene kind of the two).
    void consumeCommands(const std::vector<RenderCommand>& commands, const Camera* camera)
    {
        ++render_calls;
        last_camera = camera;
        render_cameras.push_back(camera);
        render_command_counts.push_back(commands.size());
        last_programs.clear();
        last_programs.reserve(commands.size());
        last_polygon_modes.clear();
        last_polygon_modes.reserve(commands.size());
        last_commands_authored_polygon_modes.clear();
        last_commands_authored_polygon_modes.reserve(commands.size());
        for (const auto& command : commands) {
            last_programs.push_back(command.program.get());
            last_commands_authored_polygon_modes.push_back(command.authoredState.polygonMode);
            // The EFFECTIVE mode, resolved the way a real backend does it: what the content authored, weighed
            // against the pass that draws it (the rasterisation override travels with the pass' state, not in the
            // commands - see StateNode.hpp's resolveRenderState).
            last_polygon_modes.push_back(
                resolveRenderState(command.authoredState, pass_state).state.polygonMode);
        }
    }

  public:

    // Closed-loop release recording (removePass/clearPasses -> backend).
    int target_releases = 0;
    int pass_releases = 0;
    RenderTarget* last_released_target = nullptr;
    const RenderPass* last_released_pass = nullptr;
    void releasePass(vn::raw_ptr<const RenderPass> pass) override
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

TEST(RenderEngineTest, TheFrameReportCrossesTheEngineWholeAndAbsentPartsStayAbsent)
{
    // A backend that keeps numbers: the engine hands over exactly ITS report. A backend that keeps none
    // answers with those parts DISENGAGED rather than zeroed, because "no counters" and "a frame that
    // counted nothing" are different answers - which is what the silent half below pins.
    class ReportingBackend : public MockBackend
    {
      public:
        [[nodiscard]] FrameReport frameReport() const noexcept override
        {
            FrameReport report;
            report.counters.emplace();
            report.counters->passes = 7;
            report.retention.emplace();
            report.retention->device_waits = 3;
            report.progress.emplace();
            report.progress->submitted = 11;
            report.progress->completed = 9;
            return report;
        }
    };

    // 1. No backend: nothing is measured and nothing can be claimed, so no part is engaged.
    RenderEngine engine;
    FrameReport  report = engine.frameReport();
    EXPECT_FALSE(report.counters.has_value());
    EXPECT_FALSE(report.retention.has_value());
    EXPECT_FALSE(report.progress.has_value());
    EXPECT_FALSE(engine.capabilities().render_targets);

    // 2. A backend that keeps none: the contract's default, and it SAYS so (disengaged, not zeros).
    const vn::intrusive_ptr<RenderBackend> silent(new MockBackend());
    engine.setDevice(silent);
    report = engine.frameReport();
    EXPECT_FALSE(report.counters.has_value()) << "the default is \"no counters\", not an empty frame";
    EXPECT_FALSE(report.retention.has_value());
    EXPECT_FALSE(report.progress.has_value()) << "the default is \"no clock\", not \"nothing in flight\"";

    // 3. A backend that keeps them: the whole report crosses the engine unchanged.
    const vn::intrusive_ptr<RenderBackend> reporting(new ReportingBackend());
    engine.setDevice(reporting);
    report = engine.frameReport();
    ASSERT_TRUE(report.counters.has_value());
    EXPECT_EQ(report.counters->passes, 7U);
    ASSERT_TRUE(report.retention.has_value());
    EXPECT_EQ(report.retention->device_waits, 3U);
    ASSERT_TRUE(report.progress.has_value());
    EXPECT_EQ(report.progress->submitted, 11U);
    EXPECT_EQ(report.progress->completed, 9U);
    EXPECT_EQ(report.progress->inFlight(), 2U) << "in flight is the difference, computed once in the vocabulary";

    // 4. Capabilities cross the same way: the engine's answer IS the backend's value, whole.
    EXPECT_TRUE(engine.capabilities().render_targets);
}

TEST(RenderEngineTest, EveryExecutedPassReachesTheBackendOnceAndADisabledOneDoesNot)
{
    // Every executed pass is ONE entry of the frame's value: the backend is told which pass it is (its identity
    // for retained per-pass GPU state and the marker of "active this frame", see FrameDescription::Pass), and
    // every entry is walked to its end, so nothing a pass carried is dropped on the way. A disabled pass is not
    // executed and therefore has no entry.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    auto scene  = intrusive_ptr<Scene>(new Scene());
    auto pass_a = intrusive_ptr<RenderPass>(new RenderPass());
    pass_a->setCamera(camera);
    auto pass_b = intrusive_ptr<RenderPass>(new RenderPass());
    pass_b->setCamera(camera);
    auto hidden = intrusive_ptr<RenderPass>(new RenderPass());
    hidden->setCamera(camera);
    hidden->setEnabled(false);

    engine->addPass(pass_a, scene, 0);
    engine->addPass(pass_b, scene, 1);
    engine->addPass(hidden, scene, 2);

    engine->frame(0.016);

    ASSERT_EQ(backend->pass_entries.size(), 2u);
    EXPECT_EQ(backend->pass_entries[0], pass_a.get());
    EXPECT_EQ(backend->pass_entries[1], pass_b.get());
    EXPECT_EQ(backend->entries, 2);
    EXPECT_EQ(backend->entries_closed, backend->entries);

    // Enabling the pass again brings its entry (and drawing) back.
    hidden->setEnabled(true);
    engine->frame(0.016);
    ASSERT_EQ(backend->pass_entries.size(), 5u);
    EXPECT_EQ(backend->pass_entries[2], pass_a.get());
    EXPECT_EQ(backend->pass_entries[3], pass_b.get());
    EXPECT_EQ(backend->pass_entries[4], hidden.get());
    EXPECT_EQ(backend->entries_closed, backend->entries);
}

TEST(RenderEngineTest, RemovePassReleasesBackendPerPassState)
{
    // The primary closed loop: a removed pass' retained GPU state is released
    // by identity (releasePass), not only through the legacy camera-keyed
    // window-layer call.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

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
    engine->setDevice(backend);

    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    auto pass   = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setRenderTarget(target);
    engine->addPass(pass, 1);

    engine->removePass(pass.get());
    EXPECT_EQ(backend->target_releases, 1);
    EXPECT_EQ(backend->last_released_target, target.get());
}

TEST(RenderEngineTest, RemovePassReleasesBackendStateAndTarget)
{
    // Closed loop: dropping a pass must tell the backend to free the per-pass
    // state it retained (keyed by the pass) AND any off-screen target the pass
    // owns.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

    auto camera  = intrusive_ptr<Camera>(new Camera());
    auto target  = intrusive_ptr<RenderTarget>(new RenderTarget());
    auto pass    = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(camera);
    pass->setRenderTarget(target);
    engine->addPass(pass, 1);

    engine->removePass(pass.get());
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
    engine->setDevice(backend);

    auto cam_a   = intrusive_ptr<Camera>(new Camera());
    auto pass_a  = intrusive_ptr<RenderPass>(new RenderPass());
    pass_a->setCamera(cam_a);
    engine->addPass(pass_a, 0);

    auto cam_b   = intrusive_ptr<Camera>(new Camera());
    auto target  = intrusive_ptr<RenderTarget>(new RenderTarget());
    auto pass_b  = intrusive_ptr<RenderPass>(new RenderPass());
    pass_b->setCamera(cam_b);
    pass_b->setRenderTarget(target);
    engine->addPass(pass_b, 5);

    EXPECT_EQ(engine->passCount(), 2u);
    engine->clearPasses();
    EXPECT_EQ(engine->passCount(), 0u);
    EXPECT_EQ(backend->target_releases, 1);
    EXPECT_EQ(backend->last_released_target, target.get());
    EXPECT_EQ(backend->pass_releases, 2);
}

/**
 * @brief The frame plan: what a frame of this pipeline consists of, and whether it is telling the truth.
 *
 * The plan is the answer to a question tests used to assemble by hand out of `passCount()` and pointer
 * comparisons - and, more importantly, the thing a human (or a bug report) reads to see a frame's shape.
 */
TEST(RenderEngineTest, FramePlanDescribesThePassesInTheOrderTheyWillRun)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto camera  = intrusive_ptr<Camera>(new Camera());
    auto target  = intrusive_ptr<RenderTarget>(new RenderTarget());

    auto offscreen = intrusive_ptr<RenderPass>(new RenderPass());
    offscreen->setName(u8"gbuffer");
    offscreen->setCamera(camera);
    offscreen->setRenderTarget(target);
    offscreen->setViewport(Viewport{ 10, 20, 30, 40 });
    offscreen->setClearEnabled(false);
    offscreen->addInputName(u8"SceneColor");
    offscreen->setOutputName(u8"GBuffer");

    auto present = intrusive_ptr<RenderPass>(new RenderPass());
    present->setName(u8"present");
    present->setCamera(camera);
    present->setClearColor(Color(1, 2, 3, 255));
    present->setShouldClearDepth(false);

    auto hud = intrusive_ptr<RenderPass>(new RenderPass());
    hud->setName(u8"hud");
    hud->setEnabled(false);

    engine->addPass(offscreen, content, -40);
    engine->addPass(present, content, 0);
    engine->addPass(hud, 60);

    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 3u);

    // Every fact the pass holds about itself, in the shape a reader wants it: what it draws into, through what,
    // with which content, clearing what, over which rectangle, reading and producing what.
    EXPECT_EQ(plan.passes[0].pass, offscreen.get());
    EXPECT_EQ(plan.passes[0].order, -40);
    EXPECT_EQ(plan.passes[0].name, u8"gbuffer");
    EXPECT_EQ(plan.passes[0].content, content.get());
    EXPECT_EQ(plan.passes[0].camera, camera.get());
    EXPECT_EQ(plan.passes[0].target, target.get());
    EXPECT_TRUE(plan.passes[0].enabled);
    EXPECT_FALSE(plan.passes[0].clears);
    ASSERT_TRUE(plan.passes[0].viewport.has_value());
    EXPECT_EQ(plan.passes[0].viewport->width, 30);
    ASSERT_EQ(plan.passes[0].inputs.size(), 1u);
    EXPECT_EQ(plan.passes[0].inputs[0], u8"SceneColor");
    EXPECT_EQ(plan.passes[0].output, u8"GBuffer");
    EXPECT_EQ(plan.passes[0].record_micros, 0u) << "nothing has run yet, and the plan does not pretend otherwise";

    // A pass with no target draws to the WINDOW - said by a null target, exactly as the pass itself says it.
    EXPECT_EQ(plan.passes[1].name, u8"present");
    EXPECT_EQ(plan.passes[1].target, nullptr);
    EXPECT_TRUE(plan.passes[1].clears);
    EXPECT_EQ(plan.passes[1].clear_color, Color(1, 2, 3, 255));
    EXPECT_FALSE(plan.passes[1].clears_depth);
    EXPECT_FALSE(plan.passes[1].viewport.has_value()) << "no viewport: the whole target";

    // A disabled pass is part of the frame's picture and says it will not run.
    EXPECT_EQ(plan.passes[2].name, u8"hud");
    EXPECT_FALSE(plan.passes[2].enabled);

    // ... and the table a human reads says the same things, one line per pass.
    const std::string text = plan.describe().as_std_str();
    EXPECT_NE(text.find("frame plan: 3 pass(es) (1 disabled)"), std::string::npos);
    EXPECT_NE(text.find("name='gbuffer'"), std::string::npos);
    EXPECT_NE(text.find("DISABLED"), std::string::npos);
    EXPECT_NE(text.find("clear=rgba(1,2,3,255)"), std::string::npos) << "no depth in the clear: the pass said so";
    EXPECT_NE(text.find("viewport=[10,20 30x40]"), std::string::npos);
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 3) << "one line per pass, plus the header";
}

TEST(RenderEngineTest, TheFramePlanIsWhatTheFrameActuallyRan)
{
    // The plan is not a second opinion about the frame: the passes it lists as running, in the order it lists
    // them, are exactly the passes the backend was opened scopes for. Without this the dump would be a plausible
    // story, which is worse than no dump at all.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto scene = intrusive_ptr<Scene>(new Scene());
    auto early = intrusive_ptr<RenderPass>(new RenderPass());
    early->setName(u8"gbuffer");
    auto main = intrusive_ptr<RenderPass>(new RenderPass());
    main->setName(u8"main");
    auto hidden = intrusive_ptr<RenderPass>(new RenderPass());
    hidden->setEnabled(false);
    auto late = intrusive_ptr<RenderPass>(new RenderPass());
    late->setName(u8"hud");

    engine->addPass(late, 60);
    engine->addPass(hidden, 40);
    engine->addPass(main, scene, 0);
    engine->addPass(early, scene, -40);

    engine->frame(0.016);

    std::vector<const RenderPass*> planned;
    for (const PassPlan& pass : engine->framePlan().passes) {
        if (pass.enabled) {
            planned.push_back(pass.pass);
        }
    }
    EXPECT_EQ(planned, backend->pass_entries) << "the plan's running passes, in order, are what ran";
    ASSERT_EQ(backend->pass_entries.size(), 3u);
    EXPECT_EQ(backend->pass_entries[0], early.get());
    EXPECT_EQ(backend->pass_entries[1], main.get());
    EXPECT_EQ(backend->pass_entries[2], late.get());
}

TEST(RenderEngineTest, TheFramePlanCarriesWhatEachPassLastCost)
{
    // A plan cannot know the future, so the one number that is about the past says so: how long the LAST frame's
    // work on that pass took. A pass that did not run last frame reports none.
    //
    // WHERE THAT WORK IS: the frame runs from the value the compile built (see RenderEngine::buildFrameValue), so
    // what a pass costs the frame is the hook + the inputs + the pass describing itself - collecting and culling
    // its content - and NOT the wire announcements that follow (those are the backend's cost). A pass is slow by
    // being slow to DESCRIBE itself, which is what this pins: the number still names the same thing it always did
    // (this pass' share of the frame's record), it just measures it where the record now happens.
    class SlowPass : public RenderPass
    {
      public:
        bool describe(vn::raw_ptr<Scene> scene, PassDescription& out) override
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return RenderPass::describe(scene, out);
        }
    };

    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto slow    = intrusive_ptr<SlowPass>(new SlowPass());
    slow->setName(u8"slow");
    auto skipped = intrusive_ptr<RenderPass>(new RenderPass());
    skipped->setName(u8"skipped");
    skipped->setEnabled(false);
    // A scene, so the pass really has something to describe (a pass with nothing to describe costs nothing even
    // when it is slow at saying so - and that is not what this case is about).
    auto scene = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    slow->setCamera(camera);

    engine->addPass(slow, scene, 0);
    engine->addPass(skipped, 10);

    const FramePlan before = engine->framePlan();
    ASSERT_EQ(before.passes.size(), 2u);
    EXPECT_EQ(before.passes[0].record_micros, 0u);

    engine->frame(0.016);

    const FramePlan after = engine->framePlan();
    ASSERT_EQ(after.passes.size(), 2u);
    EXPECT_GE(after.passes[0].record_micros, 1000u) << "the slow pass really did take about 2 ms";
    EXPECT_EQ(after.passes[1].record_micros, 0u) << "a pass that did not run has no cost this frame";
}

TEST(RenderEngineTest, PassOrderAnswersWhereARegisteredPassRuns)
{
    // The order lives in the ENGINE (addPass places the pass in its slot list), so this is the only way to ask
    // where a pass sits in the frame - and an answer doubles as "this pass is registered at all", which is what
    // RenderSurface's presentation table is ordered by and what a handle somebody remembers needs to know.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto camera  = intrusive_ptr<Camera>(new Camera());
    auto pass    = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(camera);

    EXPECT_FALSE(engine->passOrder(pass.get()).has_value());
    EXPECT_FALSE(engine->passOrder(nullptr).has_value());

    engine->addPass(pass, 7);
    ASSERT_TRUE(engine->passOrder(pass.get()).has_value());
    EXPECT_EQ(*engine->passOrder(pass.get()), 7);

    // A disabled pass is still in the frame's schedule: it is skipped, not unregistered.
    pass->setEnabled(false);
    EXPECT_EQ(*engine->passOrder(pass.get()), 7);

    engine->removePass(pass.get());
    EXPECT_FALSE(engine->passOrder(pass.get()).has_value());
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
    engine->setDevice(backend);

    EXPECT_TRUE(engine->initialize());
    EXPECT_TRUE(backend->ok);
    EXPECT_EQ(engine->device(), backend.get());
}

TEST(RenderEngineTest, FrameRunsPipeline)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->initialize();

    // An explicit pipeline: a window pass (order 0, null render target)
    // carrying a camera and an explicit content scene, drawing to the
    // backbuffer.
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam = intrusive_ptr<Camera>(new Camera());
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(cam);
    engine->addPass(pass, content, 0);

    engine->frame();

    EXPECT_EQ(backend->pass_entries.size(), 1u) << "the pass arrived in the frame's value";
    EXPECT_EQ(backend->entries_closed, backend->entries);
    EXPECT_EQ(backend->surface_half.present_calls, 1) << "the engine presents the frame it ran, through the surface";
    EXPECT_GE(backend->clear_calls, 1);
}

TEST(RenderEngineTest, APassAnnouncesItsClearPolicyToTheBackend)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->initialize();

    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam = intrusive_ptr<Camera>(new Camera());

    // A pass that CLEARS announces its colour and its depth flag...
    auto clearing = intrusive_ptr<RenderPass>(new RenderPass());
    clearing->setCamera(cam);
    clearing->setClearColor(Color(10, 20, 30, 255));
    clearing->setShouldClearDepth(false);
    engine->addPass(clearing, content, 0);

    // ...and a pass that does not clear announces NOTHING, even though it carries a colour of its
    // own: a backend must be able to tell the two apart, so this pass must not overwrite what the
    // first one said.
    auto loading = intrusive_ptr<RenderPass>(new RenderPass());
    loading->setCamera(cam);
    loading->setClearEnabled(false);
    loading->setClearColor(Color(200, 200, 200, 255));
    engine->addPass(loading, content, 1);

    engine->frame();

    EXPECT_EQ(backend->clear_calls, 1);
    EXPECT_EQ(backend->last_clear_policy.color.r, 10);
    EXPECT_EQ(backend->last_clear_policy.color.g, 20);
    EXPECT_EQ(backend->last_clear_policy.color.b, 30);
    EXPECT_FALSE(backend->last_clear_policy.depth);
}

TEST(RenderEngineTest, ABackendThatCannotDrawTargetsIsToldOncePerEpisode)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->initialize();

    std::vector<RenderDiagnostic> reported;
    engine->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    auto offscreen = intrusive_ptr<RenderTarget>(new RenderTarget());
    auto pass      = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setName(u8"GBuffer");
    pass->setRenderTarget(offscreen);
    engine->addPass(pass, 0);

    // This backend honours targets, so the pipeline means what it says and nothing is reported about it.
    engine->frame();
    const std::size_t quiet            = engine->diagnosticCount();
    const std::size_t reported_before  = reported.size();

    // The same pipeline on a backend that cannot draw into a target: the pass is recorded in the window
    // instead of where the pipeline put it (a wrong picture, not a slow one), so the engine says so.
    backend->supports_targets = false;
    engine->frame();
    ASSERT_EQ(engine->diagnosticCount(), quiet + 1);
    ASSERT_EQ(reported.size(), reported_before + 1);
    const RenderDiagnostic& diagnostic = reported[reported_before];
    EXPECT_EQ(diagnostic.severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.category, DiagnosticCategory::TargetBuildFailed);
    EXPECT_NE(diagnostic.message.as_std_str().find("GBuffer"), std::string::npos)
        << "the report names the pass the host has to fix";

    // ONCE: a host looping on frames cannot fix this by drawing another one, so it is an episode.
    engine->frame();
    EXPECT_EQ(engine->diagnosticCount(), quiet + 1);

    // The ask stops (the pass is skipped), which re-arms the episode: asking again is a new problem.
    pass->setEnabled(false);
    engine->frame();
    EXPECT_EQ(engine->diagnosticCount(), quiet + 1);
    pass->setEnabled(true);
    engine->frame();
    EXPECT_EQ(engine->diagnosticCount(), quiet + 2);
}

TEST(RenderEngineTest, FrameBeforeInitializeIsNoOp)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

    // Not initialized: frame should do nothing.
    engine->frame();
    EXPECT_EQ(backend->surface_half.present_calls, 0);
    EXPECT_TRUE(backend->pass_entries.empty());
}

TEST(RenderEngineTest, EngineStartsEmpty)
{
    // Design C: the engine imposes no pipeline - no content scene, no camera,
    // no registered pass until the caller binds everything explicitly.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

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

TEST(SceneViewTest, ResizingTellsTheCameraTheShapeOfThePictureItDraws)
{
    // ONE CAMERA, ONE PICTURE SHAPE (see Camera), and the view is the one place that knows the shape: the pane's
    // rectangle is the picture, so the camera is given it here - where the size arrives - rather than at the pass
    // that draws with it (which no longer has a say: the engine reports a mismatch, see
    // RenderEngine::collectPictureShapeMismatches). A camera nobody resized keeps its 1:1.
    SceneView view;
    ASSERT_NE(view.camera(), nullptr);
    EXPECT_DOUBLE_EQ(view.camera()->aspectRatio(), 1.0);

    view.onSurfaceResized(1600, 900);
    EXPECT_DOUBLE_EQ(view.camera()->aspectRatio(), 1600.0 / 900.0);

    // The same view's camera follows every size, and a size nobody can draw (nothing to divide by) is not one.
    view.onSurfaceResized(600, 800);
    EXPECT_DOUBLE_EQ(view.camera()->aspectRatio(), 600.0 / 800.0);
    view.onSurfaceResized(0, 0);
    EXPECT_DOUBLE_EQ(view.camera()->aspectRatio(), 600.0 / 800.0) << "a surface with no extent states no shape";
}

TEST(SceneViewTest, AViewDrawsThePipelineItAdopted)
{
    // A view holds no recipe: a chain IS a pipeline (built by RenderPipelineBuilder, which is where the options
    // live) and a view only adopts one (SceneView::setPipeline). So "this view presents a picture" is that two
    // steps and nothing else - before them there is no pass at all, after them exactly the pass that was built.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    EXPECT_EQ(view->pipeline(), nullptr);
    EXPECT_EQ(engine->passCount(), 0u);

    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 1u);
    EXPECT_EQ(view->pipeline(), pipeline.get());
    // The pass IS the window pass: it draws this view's camera to the backbuffer (both were given to the builder).
    ASSERT_NE(pipeline->windowPass(), nullptr);
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), view->camera()));

    // Adopting the pipeline it already has is one pipeline, and nothing is re-registered.
    view->setPipeline(pipeline);
    EXPECT_EQ(engine->passCount(), 1u);
}

TEST(SceneViewTest, AdoptingAPipelineDoesNotStandDownForSomebodyElsesPass)
{
    // "Whoever speaks first presents" is gone: a pass carrying this view's camera with a null target used to
    // suppress the view's default window pass, inferred from the engine's pass list (global, order-sensitive, and it
    // dropped the SECOND view sharing a camera). Which pass presents which SURFACE is declared on the surface now
    // (RenderSurface) - so a view draws the pipeline it adopted, and the engine's list is only where passes live.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    auto main = intrusive_ptr<RenderPass>(new RenderPass());
    main->setCamera(intrusive_ptr<Camera>(view->camera()));
    engine->addPass(main, 0);

    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 2u) << "the adopted pipeline registers its passes beside whatever is there";
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), view->camera()));
}

TEST(LightRigTest, StudioIsAmbientPlusKeyAndFill)
{
    const LightRig rig = LightRig::studio();

    ASSERT_EQ(rig.lights().size(), 3u);
    // One ambient fill, two directional lights, and every direction actually points somewhere.
    std::size_t ambient = 0;
    std::size_t directional = 0;
    for (const LightRig::Entry& entry : rig.lights()) {
        if (entry.directional) {
            ++directional;
            EXPECT_TRUE(entry.hasDirection());
        } else {
            ++ambient;
        }
    }
    EXPECT_EQ(ambient, 1u);
    EXPECT_EQ(directional, 2u);
    EXPECT_GE(rig.lights()[1].intensity, rig.lights()[2].intensity);  // key brighter than fill
}

TEST(LightRigTest, HeadlightFollowsTheDirectionItIsGiven)
{
    const LightRig rig = LightRig::headlight(vn::math::Vec3d(0.0, 0.0, 1.0));
    ASSERT_EQ(rig.lights().size(), 1u);
    EXPECT_TRUE(rig.lights()[0].directional);
    EXPECT_DOUBLE_EQ(rig.lights()[0].direction.z, 1.0);

    // A zero direction is a light that would light nothing: the rig falls back to the default view direction.
    const LightRig fallback = LightRig::headlight(vn::math::Vec3d(0.0, 0.0, 0.0));
    ASSERT_EQ(fallback.lights().size(), 1u);
    EXPECT_TRUE(fallback.lights()[0].hasDirection());
}

TEST(LightRigTest, AddToLightsTheSceneAndCreatesFreshLightsEveryTime)
{
    auto scene = intrusive_ptr<Scene>(new Scene());
    ASSERT_FALSE(scene->hasLights());

    LightRig::studio().addTo(*scene);
    EXPECT_EQ(scene->lights().size(), 3u);

    // Appending is appending: a second rig adds another set (that is what re-lighting twice means).
    LightRig::studio().addTo(*scene);
    EXPECT_EQ(scene->lights().size(), 6u);

    // The rig holds descriptions, so the two sets are different Light objects, not the same three shared twice.
    EXPECT_NE(scene->lights()[0].get(), scene->lights()[3].get());

    // Weaving the rig through the scene's own entry point is the same thing.
    scene->clearLights();
    scene->addLighting(LightRig::studio());
    EXPECT_EQ(scene->lights().size(), 3u);

    // ... and setLighting() replaces instead of appending (the explicit "this is now the lighting" spelling).
    scene->setLighting(LightRig::headlight());
    EXPECT_EQ(scene->lights().size(), 1u);
    EXPECT_TRUE(scene->lights()[0]->hasDirection());
}

TEST(LightRigTest, TheOriginalLightApiIsUnchanged)
{
    // The rig is a convenience in front of the same mechanism, not a replacement for it.
    auto scene = intrusive_ptr<Scene>(new Scene());
    auto light = intrusive_ptr<Light>(new Light());

    scene->addLight(light);
    EXPECT_EQ(scene->lights().size(), 1u);
    scene->addLighting(LightRig::headlight());
    EXPECT_EQ(scene->lights().size(), 2u);
    scene->removeLight(light.get());
    EXPECT_EQ(scene->lights().size(), 1u);
    scene->clearLights();
    EXPECT_FALSE(scene->hasLights());
}

TEST(SceneViewViewportTest, ADefaultBuildIsTheBehaviourBeforeAnyOptions)
{
    // No options at all: forward, no HUD, the whole surface - and exactly one window pass, which is what every
    // existing host and test already relied on (J2). The view holds none of it: it holds a pipeline, or nothing.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());



    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 1u);
    EXPECT_EQ(pipeline->shadingPath(), ShadingPath::Forward);
    EXPECT_EQ(pipeline->shadingMode(), ShadingMode::Lit);
    EXPECT_EQ(pipeline->gizmo(), nullptr);
    EXPECT_EQ(pipeline->fpsOverlay(), nullptr);
    EXPECT_FALSE(pipeline->isGizmoVisible());
    EXPECT_FALSE(pipeline->isStatsVisible());
    // A full viewport is what "no viewport" already means: the pass is left without one, so the default path is
    // byte-for-byte the pipeline it was before viewports existed.
    ASSERT_NE(pipeline->windowPass(), nullptr);
    EXPECT_FALSE(pipeline->windowPass()->hasViewport());
}

TEST(SceneViewViewportTest, TheBuildOptionsDecideWhichOverlayPassesExist)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    // The frame's SHAPE, not a count of it: which passes exist is the question, and the plan answers it by name.
    const auto pass_names = [&engine] {
        std::vector<String> names;
        for (const PassPlan& pass : engine->framePlan().passes) {
            names.push_back(pass.name);
        }
        return names;
    };

    PipelineOptions both;
    both.gizmo.source_camera = view->camera();
    both.fps.enabled         = true;
    auto pipeline            = buildFor(*view, engine.get(), both);
    ASSERT_NE(pipeline, nullptr);

    // Window pass + the two overlays, and both are visible as soon as they exist.
    EXPECT_EQ(pass_names(), (std::vector<String>{ u8"main", u8"axis_gizmo", u8"fps_overlay" }));
    EXPECT_NE(pipeline->gizmo(), nullptr);
    EXPECT_NE(pipeline->fpsOverlay(), nullptr);
    EXPECT_TRUE(pipeline->isGizmoVisible());
    EXPECT_TRUE(pipeline->isStatsVisible());

    // Dropping an overlay from the options takes its pass away: the next build is a different pipeline, and the one
    // it replaced takes its own registrations with it.
    PipelineOptions readout_only;
    readout_only.fps.enabled = true;
    pipeline                 = buildFor(*view, engine.get(), readout_only);
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(pass_names(), (std::vector<String>{ u8"main", u8"fps_overlay" }));
    EXPECT_EQ(pipeline->gizmo(), nullptr);
    EXPECT_TRUE(pipeline->isStatsVisible());

    // ... and one build with no overlays leaves exactly the window pass.
    pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(pass_names(), (std::vector<String>{ u8"main" }))
        << "one pass, still the window pass, and nothing else left over";
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), view->camera()));
}

TEST(SceneViewViewportTest, OverlayVisibilityIsASwitchThatNeverRebuilds)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    PipelineOptions with_readout;
    with_readout.fps.enabled = true;
    auto pipeline            = buildFor(*view, engine.get(), with_readout);
    ASSERT_NE(pipeline, nullptr);

    RenderPass* const readout = pipeline->fpsOverlayPass();
    ASSERT_NE(readout, nullptr);

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(pipeline->setStatsVisible(false));
        EXPECT_FALSE(pipeline->isStatsVisible());
        EXPECT_FALSE(readout->isEnabled());
        EXPECT_TRUE(pipeline->setStatsVisible(true));
        EXPECT_TRUE(pipeline->isStatsVisible());
        EXPECT_TRUE(readout->isEnabled());
    }

    // Nothing was rebuilt and nothing was re-registered: same handle, same pass count.
    EXPECT_EQ(view->pipeline(), pipeline.get());
    EXPECT_EQ(engine->passCount(), 2u);
    // The state lives on the pass, so a host toggling through the handle is seen by the pipeline.
    readout->setEnabled(false);
    EXPECT_FALSE(pipeline->isStatsVisible());

    // A chain that has no readout says so - once - instead of silently doing nothing.
    auto bare                     = buildFor(*view, engine.get());
    const std::size_t diagnostics = engine->diagnosticCount();
    ASSERT_NE(bare, nullptr);
    EXPECT_FALSE(bare->setStatsVisible(true));
    EXPECT_FALSE(bare->setGizmoVisible(true));
    EXPECT_EQ(engine->diagnosticCount(), diagnostics + 1u) << "one episode, one report (the second call is quiet)";
    EXPECT_FALSE(bare->isStatsVisible());
}

// 关掉的档不会因为一次重建又自己亮起来：重建是从**配方**建的（新 pass 天生 enabled），所以"使用者最后一次
// 说的可见性"要跟着重建重新落上去（见 applyOverlayIntent）。
// 关掉的档记在**那条链**上：一条全新的链天生是亮着的（它按选项建，选项里没有"使用者关过它"这件事）。让它在重建
// 之后仍然关着归**重建的那一方**（谁拆谁建谁负责把运行期选择重新落上去 —— 见 SurfaceWindow::buildViewerPipeline，
// 它重建前读的正是这条活着的链）。
TEST(SceneViewViewportTest, AHiddenOverlayIsHiddenOnTheChainItWasStatedOnAndNowhereElse)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    PipelineOptions options;
    options.fps.enabled = true;

    auto pipeline = buildFor(*view, engine.get(), options);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->isStatsVisible());

    EXPECT_TRUE(pipeline->setStatsVisible(false));
    EXPECT_FALSE(pipeline->isStatsVisible());
    ASSERT_NE(pipeline->fpsOverlayPass(), nullptr);
    EXPECT_FALSE(pipeline->fpsOverlayPass()->isEnabled());

    // 一条新链（同样的选项）是亮着的，而被关掉的那条链原样留着 —— "关过"是它自己的状态。
    auto rebuilt = buildFor(*view, engine.get(), options);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_TRUE(rebuilt->isStatsVisible()) << "重建是按选项建的：链不知道使用者关过什么";
    EXPECT_FALSE(pipeline->isStatsVisible()) << "被关掉的那条链没有被动过";
    EXPECT_EQ(view->pipeline(), rebuilt.get());
}

// 补出自己的 show/hide 之后，档从配方里去掉再回来：可见性是**使用者最后说的那一句**，不是"新 pass 天生可见"。
// 加一个档是**选项**（多一条 pass、一次建链），加完之后开关是**零重建**的旗标：两件事各钉一次。
TEST(SceneViewViewportTest, AddingAnOverlayIsABuildOptionAndShowingItIsNot)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    ASSERT_EQ(engine->passCount(), 1u);
    EXPECT_FALSE(pipeline->isStatsVisible());

    // 这条链没有读数，所以"要一个"是**另建一条**：多一条 pass，多一次建链。旧的句柄先放掉，这样 pass 表里只有
    // 新的那条链（放掉的那条析构时把自己的 pass 摘掉）。
    pipeline = nullptr;
    PipelineOptions with_readout;
    with_readout.fps.enabled = true;
    auto readout             = buildFor(*view, engine.get(), with_readout);
    ASSERT_NE(readout, nullptr);
    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_TRUE(readout->isStatsVisible());

    // 从这里开始又是旗标了：再也不会建链。"还是同一条管线"不能拿地址比（放掉的那条会把内存还回去，下一条可能正好落在
    // 那个地址上 —— 弱身份断言就是这样错的），所以这里拿住句柄比对象。
    EXPECT_TRUE(readout->setStatsVisible(false));
    EXPECT_TRUE(readout->setStatsVisible(true));
    EXPECT_EQ(view->pipeline(), readout.get());
    EXPECT_EQ(engine->passCount(), 2u);
}

TEST(SceneViewViewportTest, ThePassLevelPolygonModeOverridesTheContentAndCanBeClearedBack)
{
    // A pass can say "draw everything as edges" (RenderPass::setPolygonModeOverride) - what a view's wireframe mode is.
    // The two things worth pinning: it really reaches the commands the backend sees, and CLEARING it puts the
    // content's own state back in charge (unset is not "force fill").
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    // Content that asked for outlines itself (a StateNode subtree).
    auto root = intrusive_ptr<StateNode>(new StateNode());
    root->setPolygonMode(PolygonMode::Line);
    root->addChild(makeTriangleNode(Vec3d(0.0, 0.0, -2.0), nullptr, u8"geo"));
    view->scene()->setRoot(root);

    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    backend->ok   = true;
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto pipeline            = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    RenderPass* const window = pipeline->windowPass();
    ASSERT_NE(window, nullptr);

    // No override: the content's own Line reaches the backend (and the default is exactly that).
    engine->frame();
    ASSERT_EQ(backend->last_polygon_modes.size(), 1u);
    EXPECT_EQ(backend->last_polygon_modes[0], PolygonMode::Line);
    EXPECT_FALSE(window->polygonModeOverride().has_value());

    // Override to Fill: the pass wins over the content ("show me surfaces"), no rebuild involved.
    window->setPolygonModeOverride(PolygonMode::Fill);
    ASSERT_EQ(window->polygonModeOverride(), PolygonMode::Fill);
    engine->frame();
    ASSERT_EQ(backend->last_polygon_modes.size(), 1u);
    EXPECT_EQ(backend->last_polygon_modes[0], PolygonMode::Fill);

    // Cleared: back to the content's own Line - not to a forced Fill.
    window->clearPolygonModeOverride();
    EXPECT_FALSE(window->polygonModeOverride().has_value());
    engine->frame();
    ASSERT_EQ(backend->last_polygon_modes.size(), 1u);
    EXPECT_EQ(backend->last_polygon_modes[0], PolygonMode::Line);
}

TEST(RenderEngineTest, APassAnnouncesItsOwnStateForTheContentItDraws)
{
    // The pass side of the state table travels as ONE announcement (RenderBackend::setPassState): its depth policy
    // and its rasterisation override together. That is what lets a backend resolve each command's final state
    // without asking the pass anything (StateNode.hpp), and what lets a rasterisation switch cost no command
    // rewrite at all.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto scene  = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0.0, 0.0, -2.0), nullptr, u8"geo"));
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(intrusive_ptr<Camera>(new Camera()));
    pass->setDepthMode(DepthMode::TestOnly);
    pass->setPolygonModeOverride(PolygonMode::Line);
    engine->addPass(pass, scene, 0);

    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());
    engine->frame();

    EXPECT_EQ(backend->pass_state.depth, DepthMode::TestOnly);
    ASSERT_TRUE(backend->pass_state.polygonMode.has_value());
    EXPECT_EQ(*backend->pass_state.polygonMode, PolygonMode::Line);

    // ... and the announcement IS the state: `stateOverrides()` is what the engine sends, not a second copy.
    EXPECT_EQ(pass->stateOverrides().depth, backend->pass_state.depth);
    EXPECT_EQ(pass->stateOverrides().polygonMode, backend->pass_state.polygonMode);
    // The content authored no rasterisation, so the pass' override is what the command resolves to - and the
    // command itself was NOT rewritten to say so (nothing to rewrite: the pass state is what decides).
    ASSERT_EQ(backend->last_polygon_modes.size(), 1u);
    EXPECT_EQ(backend->last_polygon_modes[0], PolygonMode::Line);
    EXPECT_FALSE(backend->last_commands_authored_polygon_modes[0].has_value())
        << "the command still says what the CONTENT said (nothing)";
}

TEST(SceneViewViewportTest, WireframeIsAnEdgePassWithTheUnlitColourAndNoRebuild)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);

    // Held for the length of the test: "the same pipeline" then means the same OBJECT, not the same address.
    RenderPass* const window = pipeline->windowPass();
    ASSERT_NE(window, nullptr);
    const std::size_t pass_count = engine->passCount();
    ASSERT_FALSE(window->polygonModeOverride().has_value());

    EXPECT_TRUE(pipeline->setShadingMode(ShadingMode::Wireframe));
    EXPECT_EQ(view->pipeline(), pipeline.get()) << "换到线框不该重建管线";
    EXPECT_EQ(engine->passCount(), pass_count);
    ASSERT_TRUE(window->polygonModeOverride().has_value());
    EXPECT_EQ(*window->polygonModeOverride(), PolygonMode::Line);
    ASSERT_NE(window->programOverride(), nullptr);
    EXPECT_NE(window->programOverride()->name().find(u8"builtin_forward_unlit"), vn::String::npos)
        << "线框用无光照的表面色（这就是 wireframe 的惯例：看结构，不看明暗）";

    // Back to Lit: BOTH overrides go away (and again with no rebuild).
    EXPECT_TRUE(pipeline->setShadingMode(ShadingMode::Lit));
    EXPECT_EQ(view->pipeline(), pipeline.get());
    EXPECT_FALSE(window->polygonModeOverride().has_value());
    EXPECT_EQ(window->programOverride(), nullptr);
}

TEST(SceneViewViewportTest, TheShadingModeIsAProgramOverrideNotARebuild)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);

    // Held while the "no rebuild" claims are checked: identity here means the same OBJECT, so the claim cannot
    // pass on a recycled address.
    RenderPass* const window = pipeline->windowPass();
    ASSERT_NE(window, nullptr);
    const std::size_t pass_count = engine->passCount();

    // Lit is the content's own program: no override at all (the default path is untouched).
    EXPECT_EQ(pipeline->shadingMode(), ShadingMode::Lit);
    EXPECT_EQ(window->programOverride(), nullptr);

    // The four modes are three overrides plus Lit - and every one of them is a PASS property: same pipeline, same
    // registered passes, so the switch costs an assignment instead of a build.
    for (const auto mode : { ShadingMode::Flat, ShadingMode::Unlit, ShadingMode::Normals, ShadingMode::Lit }) {
        EXPECT_TRUE(pipeline->setShadingMode(mode));
        EXPECT_EQ(pipeline->shadingMode(), mode);
        EXPECT_EQ(view->pipeline(), pipeline.get()) << "换着色模式不该重建管线";
        EXPECT_EQ(engine->passCount(), pass_count) << "换着色模式不该动 pass 表";
        const bool lit = mode == ShadingMode::Lit;
        EXPECT_EQ(window->programOverride() == nullptr, lit) << "Lit 用内容自己的程序，其余三种是覆盖";
    }

    // ... and the override is a REAL program (a name the backend diagnostics can name), not an empty placeholder.
    EXPECT_TRUE(pipeline->setShadingMode(ShadingMode::Unlit));
    ASSERT_NE(window->programOverride(), nullptr);
    EXPECT_NE(window->programOverride()->name().find(u8"builtin_forward"), vn::String::npos);

    // Changing the STRUCTURE is what costs a build (so the two costs stay separated). The old handle is released
    // first, because while it is held the replaced pipeline keeps its own passes registered (its destructor is what
    // unregisters them) and the pass count below would be measuring the wrong thing.
    pipeline = nullptr;
    PipelineOptions with_readout;
    with_readout.fps.enabled = true;
    auto rebuilt             = buildFor(*view, engine.get(), with_readout);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_EQ(engine->passCount(), pass_count + 1) << "结构变了才建链（读数档多出一条 pass）";
    EXPECT_EQ(rebuilt->shadingMode(), ShadingMode::Lit) << "重建是按选项建的：运行期那一档不跟着走";
}

// 延迟配方 + 非 Lit 档：模式是**几何阶段**的覆盖，而延迟配方画的最后一步是全屏光照（没有几何可着色）——
// 所以这一档期间链条按**前向**建（配方字段不动），于是四档在两条配方上都能画出来。切档因此是**一次重建**
// （链条变了），回到 Lit 再建回延迟；这与"同一链条上换档零重建"是两条规则，各钉一条。
// 延迟选项 + 非 Lit 档：模式是**几何阶段**的覆盖，而延迟链画的最后一步是全屏光照（没有几何可着色）——所以这一档
// 期间链按**前向**建（选项字段不动，`shadingPath()` 记的是**实际建成的**那条链），回到 Lit 就再建回延迟。这是
// "选项 → 链"的一次性决定（RenderPipelineBuilder::effectivePath），与"同一条链上换档零重建"是两条规则，各钉一条。
TEST(SceneViewViewportTest, ANonLitModeOnADeferredRecipeBuildsTheForwardChain)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    PipelineOptions options;
    options.path  = ShadingPath::Deferred;
    options.mode  = ShadingMode::Unlit;
    auto pipeline = buildFor(*view, engine.get(), options);
    ASSERT_NE(pipeline, nullptr);

    // 选项还是用户写的那个（Deferred），建成的链是前向：没有离屏目标、没有 G-buffer，画面由那条几何 pass 直接产出。
    EXPECT_EQ(pipeline->shadingPath(), ShadingPath::Forward) << "记的是实际建成的链，不是选项里写的那个";
    EXPECT_EQ(pipeline->shadingMode(), ShadingMode::Unlit);
    EXPECT_EQ(pipeline->offscreenTarget(), nullptr) << "非 Lit 档不需要 G-buffer";
    RenderPass* const window = pipeline->windowPass();
    ASSERT_NE(window, nullptr);
    ASSERT_NE(window->programOverride(), nullptr) << "画面由这条 pass 用无光照程序产出";
    EXPECT_NE(window->programOverride()->name().find(u8"builtin_forward_unlit"), vn::String::npos);

    // 线框：同一处再叠一个光栅化覆盖（这就是"四级像素之外还画得出边"的那一半）。
    EXPECT_TRUE(pipeline->setShadingMode(ShadingMode::Wireframe));
    ASSERT_TRUE(window->polygonModeOverride().has_value()) << "线框档必须让几何真的画成线";
    EXPECT_EQ(*window->polygonModeOverride(), PolygonMode::Line);

    // 回到 Lit ⇒ 链切回延迟：G-buffer 与离屏目标回来了，pass 上的两个覆盖都清掉。旧的句柄先放掉：链析构会把自己的
    // pass 摘掉，这样下面数的才只是新那条链。
    pipeline      = nullptr;
    options.mode  = ShadingMode::Lit;
    auto deferred = buildFor(*view, engine.get(), options);
    ASSERT_NE(deferred, nullptr);
    EXPECT_EQ(deferred->shadingPath(), ShadingPath::Deferred);
    EXPECT_NE(deferred->offscreenTarget(), nullptr) << "Lit 档回到延迟：G-buffer 是它的输入";
    RenderPass* const lit = deferred->windowPass();
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->programOverride(), nullptr);
    EXPECT_FALSE(lit->polygonModeOverride().has_value());

    // 来回切链走的是"先拆后建"：不该留下重复注册的 pass（拿一个同配置的新视图做对照）。
    auto fresh_engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto fresh        = intrusive_ptr<SceneView>(new SceneView());
    fresh->setEngine(fresh_engine.get());
    ASSERT_NE(buildFor(*fresh, fresh_engine.get(), options), nullptr);
    EXPECT_EQ(engine->passCount(), fresh_engine->passCount()) << "几次切换之后不该有残留的 pass";
}

// 链条没变时换档**不重建**（延迟配方 + 非 Lit 档之间的切换都在前向链条上）。
// 链没变时换档**不重建**（延迟选项 + 非 Lit 档之间：链都是那条前向的几何链，`shadingPath()` 说了这件事）。
TEST(SceneViewViewportTest, SwitchingBetweenNonLitModesOnADeferredRecipeKeepsTheChain)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    PipelineOptions options;
    options.path  = ShadingPath::Deferred;
    options.mode  = ShadingMode::Unlit;
    auto pipeline = buildFor(*view, engine.get(), options);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_EQ(pipeline->shadingPath(), ShadingPath::Forward);

    const std::size_t passes = engine->passCount();

    EXPECT_TRUE(pipeline->setShadingMode(ShadingMode::Normals));
    EXPECT_EQ(view->pipeline(), pipeline.get()) << "同一条链上换档不该重建";
    EXPECT_EQ(engine->passCount(), passes);
    EXPECT_NE(pipeline->windowPass()->programOverride()->name().find(u8"builtin_forward_normals"),
              vn::String::npos);

    EXPECT_TRUE(pipeline->setShadingMode(ShadingMode::Wireframe));
    EXPECT_EQ(view->pipeline(), pipeline.get());
    EXPECT_EQ(engine->passCount(), passes);
}

TEST(SceneViewViewportTest, TheAreaSizeDrivesAspectAndLayoutSteps)
{
    // One area, one picture: what the host reports is the AREA's size, and everything this view derives comes out of
    // that one number - the layout steps, the camera's shape, the overlays' boxes. There is no second rectangle
    // anywhere (that was the multi-pane half, dropped: a host that wants two pictures brings its own passes and
    // their own sub-viewports).
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    PipelineOptions options;
    options.gizmo.source_camera = view->camera();
    options.fps.enabled         = true;
    auto pipeline               = buildFor(*view, engine.get(), options);
    ASSERT_NE(pipeline, nullptr);

    int    layout_w = 0;
    int    layout_h = 0;
    int    layout_ratio_calls = 0;
    double layout_ratio    = 0.0;
    view->addSurfaceLayout([&](int w, int h) {
        layout_w = w;
        layout_h = h;
    });
    view->addSurfaceLayout([&](int w, int h, double ratio) {
        layout_w = w;
        layout_h = h;
        ++layout_ratio_calls;
        layout_ratio = ratio;
    });

    const std::size_t pass_count_before = engine->passCount();

    view->onSurfaceResized(1280, 720, 2.0);

    EXPECT_EQ(layout_w, 1280);
    EXPECT_EQ(layout_h, 720);
    EXPECT_EQ(layout_ratio_calls, 1);
    EXPECT_DOUBLE_EQ(layout_ratio, 2.0);
    // The picture's SHAPE is the area's (one camera, one shape - see Camera).
    EXPECT_DOUBLE_EQ(view->camera()->aspectRatio(), 1280.0 / 720.0);

    // The picture IS the area: the window pass keeps no rectangle of its own (what a pass without a viewport already
    // means), so the default path is byte-for-byte the pipeline it was before viewports existed. A host that wants a
    // sub-rectangle states one on the pass itself, in device pixels (RenderPass::setViewport).
    RenderPass* const window = pipeline->windowPass();
    ASSERT_NE(window, nullptr);
    EXPECT_FALSE(window->hasViewport());

    // The overlays land inside the AREA (they anchor to the corner of their area, in device pixels).
    ASSERT_NE(pipeline->fpsOverlay(), nullptr);
    const Viewport readout = pipeline->fpsOverlayPass()->viewport();
    EXPECT_GT(readout.width, 0);
    EXPECT_LE(readout.x + readout.width, 1280 * 2);
    EXPECT_LE(readout.y + readout.height, 720 * 2);

    // Geometry is not recipe: no rebuild happened, the very same pipeline is still there and nothing was
    // re-registered (whatever the recipe's pass count is, it did not change).
    EXPECT_EQ(view->pipeline(), pipeline.get());
    EXPECT_EQ(engine->passCount(), pass_count_before);

    // A second announcement goes through the same one path (and keeps the same pipeline).
    view->onSurfaceResized(800, 600, 1.0);
    EXPECT_EQ(layout_w, 800);
    EXPECT_EQ(layout_h, 600);
    EXPECT_DOUBLE_EQ(view->camera()->aspectRatio(), 800.0 / 600.0);
    EXPECT_EQ(view->pipeline(), pipeline.get());
}

TEST(SceneViewViewportTest, TheAreaSizeHasOneOwnerAndTheViewReadsIt)
{
    // One owner for the size of the thing a picture ends up in (S6/S9): while a view presents an area, the AREA's
    // number is the one every consumer derives from, and the host feeding the view its own is not read. A view that
    // presents nothing is on its own and behaves exactly as it always did.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    // 1) No area: the host's own announcement is the only size (a view used on its own is a legal configuration).
    view->onSurfaceResized(100, 100, 1.0);
    EXPECT_DOUBLE_EQ(view->camera()->aspectRatio(), 1.0);

    // 2) Presented: the area's number wins (800x400 = 2:1), the view's own 100x100 stops being read.
    ASSERT_NE(buildFor(*view, engine.get()), nullptr);
    surface->resize(800, 400, 1.0);
    ASSERT_TRUE(surface->present(*view));
    EXPECT_DOUBLE_EQ(view->camera()->aspectRatio(), 2.0);
    EXPECT_EQ(view->surfaceWidth(), 800);
    EXPECT_EQ(view->surfaceHeight(), 400);
    EXPECT_DOUBLE_EQ(view->surfacePixelRatio(), 1.0);

    // 3) The area changes: the view was fed nothing and follows anyway - the measured form of "no second copy".
    surface->resize(320, 400, 1.0);
    EXPECT_DOUBLE_EQ(view->camera()->aspectRatio(), 0.8);

    // 4) Dismissed: the area hands its last size over to the view, which keeps drawing where it was rather than
    //    jumping back to the number from before it was presented.
    ASSERT_TRUE(surface->dismiss(view.get()));
    EXPECT_EQ(view->surfaceWidth(), 320);
    EXPECT_EQ(view->surfaceHeight(), 400);
    surface->resize(1280, 720, 1.0);
    EXPECT_EQ(view->surfaceWidth(), 320) << "an area no longer feeds a view it does not present";

    // 5) Its own announcement is the size again.
    view->onSurfaceResized(300, 600, 1.0);
    EXPECT_DOUBLE_EQ(view->camera()->aspectRatio(), 0.5);

    // 6) A view presented on an already-sized area is laid out at once - the host feeds nothing (the area remembers,
    //    and present() is what makes the view read it).
    auto later   = intrusive_ptr<SceneView>(new SceneView());
    later->setEngine(engine.get());
    ASSERT_NE(buildFor(*later, engine.get()), nullptr);
    int later_w = 0;
    int later_h = 0;
    later->addSurfaceLayout([&](int w, int h) {
        later_w = w;
        later_h = h;
    });
    ASSERT_TRUE(surface->present(*later));
    EXPECT_EQ(later_w, 1280) << "the layout step receives the area's size at the moment the view is presented";
    EXPECT_EQ(later_h, 720);
}

TEST(SceneViewViewportTest, TheOverlayBoxSitsInTheAreaCorner)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    PipelineOptions options;
    options.gizmo.source_camera = view->camera();
    auto pipeline               = buildFor(*view, engine.get(), options);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(pipeline->gizmo(), nullptr);

    view->onSurfaceResized(400, 300, 1.0);

    // The default gizmo still sits in the bottom-left corner of the surface with its 16 px margin: a full
    // viewport must not move anything.
    const Viewport box = pipeline->gizmoPass()->viewport();
    EXPECT_EQ(box.x, 16);
    EXPECT_EQ(box.y, 300 - 16 - 96);
    EXPECT_EQ(box.width, 96);
}

// 应用自己上屏时，视图**不被建链** —— 那条"表里有声明就不建自己的链"的规则归渲染控件（SurfaceWindow::init 先看
// 表），于是这个视图什么也不画。而把**没有链**的视图声明为呈现是**被拒**的（表把声明解析成"视图采纳的那条链的
// pass"，没有链就没有东西可指），拒绝会说出来 —— 不然宿主会以为窗口正在被画。
TEST(SceneViewViewportTest, AnApplicationWindowPassLeavesTheViewWithNothingToDeclare)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));

    // The application presents this surface itself (its own deferred main pass) and DECLARES it.
    auto main = intrusive_ptr<RenderPass>(new RenderPass());
    main->setCamera(intrusive_ptr<Camera>(view->camera()));
    engine->addPass(main, 0);
    ASSERT_TRUE(surface->present(main.get()));
    ASSERT_TRUE(surface->hasPresentation());

    // The view adopted nothing, so nothing was built for it and there is no picture of its own.
    EXPECT_EQ(engine->passCount(), 1u);
    EXPECT_EQ(view->pipeline(), nullptr);

    const std::size_t diagnostics = engine->diagnosticCount();
    EXPECT_FALSE(surface->present(*view)) << "a declaration has to point at a pass of the pipeline the view adopted";
    EXPECT_GT(engine->diagnosticCount(), diagnostics) << "and it says so instead of silently accepting";
}

TEST(SceneViewViewportTest, RebuildingAfterTheHostReleasedTheEngineIsSafe)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);

    // The host drops its reference: the view borrows the engine, so from here on the only thing keeping it alive is
    // the pipeline the view adopted (a Pipeline holds its engine strongly - its passes live in that engine's list).
    auto* borrowed = view->engine();
    engine         = nullptr;

    PipelineOptions with_readout;
    with_readout.fps.enabled = true;
    auto rebuilt             = buildFor(*view, borrowed, with_readout);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_EQ(view->pipeline(), rebuilt.get());
    EXPECT_TRUE(rebuilt->isStatsVisible());
    EXPECT_FALSE(rebuilt->windowPass()->hasViewport());

    // The view is sized without the engine being asked (its own announcement is the size it derives from).
    view->onSurfaceResized(800, 600, 1.0);
    EXPECT_DOUBLE_EQ(view->camera()->aspectRatio(), 800.0 / 600.0);
    EXPECT_EQ(view->surfaceWidth(), 800);
    EXPECT_FALSE(rebuilt->windowPass()->hasViewport()) << "the picture is the area: no rectangle of its own";
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

TEST(SceneViewTest, SetSceneReachesDefaultWindowPassAndSceneAwareManipulator)
{
    // Replacing the view's scene must reach BOTH consumers the view created:
    // the adopted pipeline's window pass (whose content is bound to the PASS, not to the
    // view) and a scene-aware manipulator, which holds a NON-OWNING scene
    // pointer and would otherwise navigate a scene the host may have dropped
    // (a use-after-free once its last reference goes away).
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

    auto view = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    // Both scenes get a root: collectRenderCommands() only counts a walk when
    // there is something to walk, so the count below is the observable.
    auto original = view->scene();
    original->setRoot(intrusive_ptr<Group>(new Group()));

    auto pipeline = buildFor(*view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    ASSERT_EQ(engine->passCount(), 1u);
    auto* orbit = dynamic_cast<OrbitCameraManipulator*>(view->manipulator());
    ASSERT_NE(orbit, nullptr);
    EXPECT_EQ(orbit->scene(), original.get());

    auto replacement = intrusive_ptr<Scene>(new Scene());
    replacement->setRoot(intrusive_ptr<Group>(new Group()));
    const auto original_walks = original->contentCollectCount();

    view->setScene(replacement);
    EXPECT_EQ(view->scene().get(), replacement.get());
    EXPECT_EQ(orbit->scene(), replacement.get());

    // One frame: the pipeline's window pass must now walk the REPLACEMENT scene
    // (its bound content) and no longer the scene it replaced.
    ASSERT_TRUE(engine->initialize());
    engine->frame(0.016);
    EXPECT_EQ(replacement->contentCollectCount(), 1u);
    EXPECT_EQ(original->contentCollectCount(), original_walks);
}

TEST(RenderEngineTest, DefaultContentProgramForwardedToBackend)
{
    // The program program-less content is shaded with is held by the engine and forwarded to the
    // backend. There is no shading-model lookup: the engine's default is the NAMED forward program
    // (chosen once in the constructor), and whatever the host sets is what the backend is told —
    // both at initialize() and immediately on a running session, so the engine's accessor and the
    // picture cannot disagree.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    ASSERT_NE(engine->defaultContentProgram(), nullptr);
    EXPECT_NE(engine->defaultContentProgram(), nullptr);

    const auto flat = vn::graphics::flatForwardProgram();
    engine->setDefaultContentProgram(flat);
    EXPECT_EQ(engine->defaultContentProgram(), flat);

    engine->setDevice(backend);
    EXPECT_TRUE(engine->initialize());
    EXPECT_EQ(backend->default_content_program_sets, 1);
    EXPECT_EQ(backend->last_default_content_program, flat);

    // A running session is told too (the backend rebuilds its shading side), and "none" is a legal
    // answer: content without its own program is then reported and skipped, not shaded with a guess.
    engine->setDefaultContentProgram(nullptr);
    EXPECT_EQ(backend->default_content_program_sets, 2);
    EXPECT_EQ(backend->last_default_content_program, nullptr);
    EXPECT_EQ(engine->defaultContentProgram(), nullptr);
}

TEST(RenderEngineTest, RegisteredPassesRunInAscendingOrder)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
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
    main->setCamera(window_cam);
    auto pre = intrusive_ptr<RenderPass>(new RenderPass());
    pre->setCamera(pre_cam);
    auto post = intrusive_ptr<RenderPass>(new RenderPass());
    post->setCamera(post_cam);

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
    engine->setDevice(backend);
    engine->initialize();

    auto sceneA = intrusive_ptr<Scene>(new Scene());
    auto sceneB = intrusive_ptr<Scene>(new Scene());
    auto cam    = intrusive_ptr<Camera>(new Camera());
    cam->setName(u8"pass-view");

    // Explicit content: pass A draws sceneA; pass B has no content.
    auto passA = intrusive_ptr<RenderPass>(new RenderPass());
    passA->setCamera(cam);
    engine->addPass(passA, sceneA, 5);
    auto passB = intrusive_ptr<RenderPass>(new RenderPass());
    passB->setCamera(cam);
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
    EXPECT_TRUE(vp.isValid());

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
    engine->setDevice(backend);
    engine->initialize();

    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(320, 180);
    auto cam  = intrusive_ptr<Camera>(new Camera());
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(cam);
    pass->setRenderTarget(target);
    pass->setViewport(10, 20, 100, 50);
    engine->addPass(pass, 5);

    // The size the passes lay themselves out on is the AREA's announcement (see RenderSurface).
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));
    area->resize(1600, 900);

    // The pass's viewport and the target size are untouched by the engine.
    EXPECT_EQ(target->width(), 320);
    EXPECT_EQ(target->height(), 180);
    ASSERT_TRUE(pass->hasViewport());
    const Viewport vp = pass->viewport();
    EXPECT_EQ(vp.x, 10);
    EXPECT_EQ(vp.y, 20);
    EXPECT_EQ(vp.width, 100);
    EXPECT_EQ(vp.height, 50);
}

TEST(RenderEngineTest, NamedOutputRegistryPublishResolve)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
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
    engine->setDevice(backend);
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
    off->setCamera(cam);
    off->setRenderTarget(rt);
    off->setOutputName(u8"SceneColor");
    engine->addPass(off, -2);

    // Consumer: a ScreenPass declaring it wants the published "SceneColor", and NAMING the program it
    // draws it with — a screen pass has no implicit shading (see ScreenPass).
    auto copy_program = vn::graphics::screenCopyProgram();
    auto screen       = intrusive_ptr<ScreenPass>(new ScreenPass());
    screen->setCamera(cam);
    screen->setProgram(copy_program);
    screen->addInputName(u8"SceneColor");
    // A screen pass composites over existing content, so it never clears.
    EXPECT_FALSE(screen->isClearEnabled());
    engine->addPass(screen, 100);

    const int before = backend->program_draws;
    engine->frame();

    // The ScreenPass resolved "SceneColor" to the off-screen target and asked
    // the backend to composite it exactly once this frame, through its program.
    EXPECT_EQ(backend->program_draws - before, 1);
    EXPECT_EQ(backend->last_program_source, rt.get());
    EXPECT_EQ(backend->last_program, copy_program.get());
    EXPECT_EQ(screen->sourceTarget(), rt.get());
    // The registry holds the off-screen target after the producer ran.
    EXPECT_EQ(engine->resolve(u8"SceneColor"), rt.get());

    // Without a producer nothing resolves, so the ScreenPass draws nothing.
    auto engine2  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend2 = intrusive_ptr<MockBackend>(new MockBackend());
    engine2->setDevice(backend2);
    engine2->initialize();
    auto orphan = intrusive_ptr<ScreenPass>(new ScreenPass());
    orphan->setCamera(cam);
    orphan->setProgram(vn::graphics::screenCopyProgram());
    orphan->addInputName(u8"SceneColor");
    engine2->addPass(orphan, 100);
    engine2->frame();
    EXPECT_EQ(backend2->program_draws, 0);
    EXPECT_EQ(orphan->sourceTarget(), nullptr);

    // A ScreenPass with NO program is the other half of the same rule: it draws nothing, and the
    // engine says so (once) instead of a backend picking a shading for it.
    auto engine3  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend3 = intrusive_ptr<MockBackend>(new MockBackend());
    engine3->setDevice(backend3);
    engine3->initialize();
    auto program_less = intrusive_ptr<ScreenPass>(new ScreenPass());
    program_less->setCamera(cam);
    program_less->addInputName(u8"SceneColor");
    engine3->publish(u8"SceneColor", rt);
    engine3->addPass(program_less, 100);
    EXPECT_EQ(engine3->diagnosticCount(), 0u);
    engine3->frame();
    EXPECT_EQ(backend3->program_draws, 0);
    EXPECT_EQ(engine3->diagnosticCount(), 1u);
    EXPECT_EQ(program_less->sourceTarget(), rt.get());   // it resolved the source, and still drew nothing
}

// ============ The pass description value ============

// S1 的判据：一条 pass **描述出来的值**与它真的发到线上的那串调用逐项相等。两边读同一份记录型后端 —— 一边是
// 引擎今天走的 execute()（线），一边把它描述的 PassDescription（值）；描述里少填一项、或走错一条调用，这条就红。
TEST(PassDescriptionTest, WhatAScenePassDescribesIsWhatItAnnounces)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    // 内容：一颗三角形 + 一组非空的灯（灯非空才有意义：空列表也会被公告一次）
    auto scene = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"tri"));
    scene->addLighting(LightRig::studio());

    auto cam = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*cam);

    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setSize(64, 36);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);

    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    // 这一格的形状写在 CAMERA 上（一个相机一张形状）：pass 的矩形只决定"画在哪"，形状是相机的状态。
    cam->setAspect(32.0 / 18.0);
    pass->setCamera(cam);
    pass->setRenderTarget(target);
    pass->setViewport(4, 2, 32, 18);
    pass->setClearColor(vn::Color(10, 20, 30));
    pass->setShouldClearDepth(false);
    pass->setDepthMode(DepthMode::TestOnly);
    pass->setPolygonModeOverride(PolygonMode::Line);
    pass->setProgramOverride(vn::graphics::unlitForwardProgram());  // 走"改写命令"那条路

    // 线上的一遍：宿主自己驱一条 pass —— 描述 + 只含这一条 pass 的帧值（见 RenderPass::execute）
    pass->execute(scene.get(), backend.get());

    // 值：同一件事描述成 PassDescription。
    vn::graphics::PassDescription described;
    ASSERT_TRUE(pass->describe(scene.get(), described));

    // —— pass 自己的四项事实 ——
    ASSERT_FALSE(backend->target_history.empty());
    EXPECT_EQ(described.target, backend->target_history.back()) << "目标：描述说的就是后端收到的";
    ASSERT_TRUE(described.viewport.has_value());
    EXPECT_EQ(described.viewport->x, backend->last_viewport[0]);
    EXPECT_EQ(described.viewport->y, backend->last_viewport[1]);
    EXPECT_EQ(described.viewport->width, backend->last_viewport[2]);
    EXPECT_EQ(described.viewport->height, backend->last_viewport[3]);
    ASSERT_TRUE(described.clear.has_value());
    EXPECT_EQ(backend->clear_calls, 1);
    EXPECT_EQ(described.clear->color, backend->last_clear_policy.color);
    EXPECT_EQ(described.clear->depth, backend->last_clear_policy.depth);
    EXPECT_EQ(described.state.depth, backend->pass_state.depth);
    EXPECT_EQ(described.state.polygonMode, backend->pass_state.polygonMode);
    EXPECT_DOUBLE_EQ(cam->aspectRatio(), 32.0 / 18.0)
        << "形状是 CAMERA 的状态（描述里没有这个字段），投影与剔除读的是同一个数";

    // —— 一次绘制：相机、命令、灯，以及走的是哪条调用 ——
    ASSERT_EQ(described.draws.size(), 1u);
    const vn::graphics::DrawDescription& draw = described.draws.front();
    EXPECT_EQ(draw.kind, vn::graphics::DrawDescription::Kind::Scene);
    ASSERT_FALSE(backend->render_cameras.empty());
    EXPECT_EQ(draw.camera, backend->render_cameras.back());
    ASSERT_NE(draw.commands, nullptr);
    EXPECT_FALSE(draw.commands->empty()) << "三角形必须在里面，否则这条只验证了空集合相等";
    EXPECT_EQ(draw.commands->size(), backend->render_command_counts.back());
    ASSERT_TRUE(draw.lights.has_value()) << "有相机 ⇒ 灯一定被公告（哪怕是空的）";
    EXPECT_EQ(draw.lights->size(), backend->last_light_count);
    EXPECT_EQ(backend->render_calls, 1);
    EXPECT_EQ(backend->program_draws, 0) << "场景档走 render()，不走全屏程序";
    // 程序覆盖是"改写命令"那条路（见 setProgramOverride）：描述里那些命令带的 program 就是它，后端看到的也是它。
    EXPECT_EQ(draw.commands->front().program, pass->programOverride());
    ASSERT_FALSE(backend->last_programs.empty());
    EXPECT_EQ(backend->last_programs.front(), pass->programOverride());
}

TEST(PassDescriptionTest, WhatAScreenPassDescribesIsWhatItAnnounces)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto cam = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*cam);

    auto source = intrusive_ptr<RenderTarget>(new RenderTarget());
    source->setSize(64, 36);
    source->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto program = vn::graphics::screenCopyProgram();
    auto pass    = intrusive_ptr<ScreenPass>(new ScreenPass());
    pass->setCamera(cam);
    pass->setProgram(program);
    pass->setViewport(0, 0, 16, 16);
    pass->resolveInputTextures({ source.get() });

    pass->execute(nullptr, backend.get());  // 没有场景：只有灯会读到它

    vn::graphics::PassDescription described;
    ASSERT_TRUE(pass->describe(nullptr, described)) << "有场景不是它的门禁：解析出源才是";

    ASSERT_EQ(described.draws.size(), 1u);
    const vn::graphics::DrawDescription& draw = described.draws.front();
    EXPECT_EQ(draw.kind, vn::graphics::DrawDescription::Kind::Screen);
    EXPECT_EQ(draw.source, source.get());
    EXPECT_EQ(draw.program.get(), program.get());
    EXPECT_EQ(draw.camera, cam.get());
    EXPECT_FALSE(draw.lights.has_value()) << "没有场景 ⇒ 不公告灯（\"没有场景\"不是关于光照的陈述）";
    EXPECT_EQ(backend->light_sets, 0);
    // 线上走的真的是全屏程序那条路，参数就是描述里那几个
    EXPECT_EQ(backend->program_draws, 1);
    EXPECT_EQ(backend->last_program_source, draw.source);
    EXPECT_EQ(backend->last_program, draw.program.get());
    EXPECT_EQ(backend->last_program_camera, draw.camera);
    EXPECT_EQ(backend->render_calls, 0) << "全屏档走 drawScreenProgram()，不走 render()";
    // 清屏在构造里就关了：描述里没有 clear，线上也没收到 setClearPolicy（0 值不是"清了黑色"）
    EXPECT_FALSE(described.clear.has_value());
    EXPECT_EQ(backend->clear_calls, 0);
}

// "这条 pass 什么都不发"与"发了目标但没画"是两件事，且都要与今天一模一样：前者一个调用都不许发（连目标都不许 ——
// 一条没在画的 pass 不许宣称清屏、堆叠或占着目标），后者照发目标 / 视口 / 清屏，只是没有绘制。
TEST(PassDescriptionTest, APassWithNothingToIssueAnnouncesNothingAtAll)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setSize(64, 36);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);

    // ① 场景档 + 没有场景：什么都不发，描述说自己什么都不发
    auto no_scene = intrusive_ptr<RenderPass>(new RenderPass());
    no_scene->setRenderTarget(target);
    vn::graphics::PassDescription described;
    EXPECT_FALSE(no_scene->describe(nullptr, described));
    no_scene->execute(nullptr, backend.get());
    EXPECT_TRUE(backend->target_history.empty()) << "一个调用都不许发，连目标都不许";
    EXPECT_EQ(backend->clear_calls, 0);
    EXPECT_EQ(backend->render_calls, 0);

    // ② 全屏档 + 源没解析出来：同上
    auto no_source = intrusive_ptr<ScreenPass>(new ScreenPass());
    no_source->setProgram(vn::graphics::screenCopyProgram());
    no_source->setRenderTarget(target);
    EXPECT_FALSE(no_source->describe(nullptr, described));
    no_source->execute(nullptr, backend.get());
    EXPECT_TRUE(backend->target_history.empty());
    EXPECT_EQ(backend->program_draws, 0);

    // ③ 场景档 + 有场景但没有相机：照发目标与状态，只是没有绘制（与今天一致）
    auto scene     = intrusive_ptr<Scene>(new Scene());
    auto no_camera = intrusive_ptr<RenderPass>(new RenderPass());
    no_camera->setRenderTarget(target);
    no_camera->execute(scene.get(), backend.get());
    EXPECT_EQ(backend->target_history.size(), 1u) << "它还在这一帧里：目标照发";
    EXPECT_EQ(backend->render_calls, 0) << "没有相机就没有绘制，也不会变成别的东西的绘制";
    vn::graphics::PassDescription camera_less;
    ASSERT_TRUE(no_camera->describe(scene.get(), camera_less));
    EXPECT_TRUE(camera_less.draws.empty()) << "描述用空 draws 说同一件事";
}

TEST(RenderEngineTest, ShutdownReleasesBackend)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->initialize();
    EXPECT_TRUE(backend->ok);

    engine->shutdown();
    EXPECT_FALSE(backend->ok);
}

TEST(RenderEngineTest, ScreenPassProgramCarriesTheAttachmentItReads)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
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
    producer->setCamera(cam);
    producer->setRenderTarget(rt);
    producer->setOutputName(u8"GBuffer");
    engine->addPass(producer, -2);

    // Two consumers reading different colour attachments of the same target: the ATTACHMENT is the
    // program's sampler binding (the fullscreen program ABI — binding i reads attachment i), so the two
    // passes differ by the program they name, not by a backend-side index.
    auto albedo_program = vn::graphics::screenCopyProgram(0);
    auto normal_program = vn::graphics::screenCopyProgram(1);
    EXPECT_NE(albedo_program->name(), normal_program->name());

    auto albedo = intrusive_ptr<ScreenPass>(new ScreenPass());
    albedo->setCamera(cam);
    albedo->setProgram(albedo_program);
    albedo->addInputName(u8"GBuffer");
    engine->addPass(albedo, 100);

    auto normal = intrusive_ptr<ScreenPass>(new ScreenPass());
    normal->setCamera(cam);
    normal->setProgram(normal_program);
    normal->addInputName(u8"GBuffer");
    engine->addPass(normal, 101);

    const int before = backend->program_draws;
    engine->frame();

    // Both resolved the published target (the MRT target is handed over, not copied) and each drew
    // through its own program.
    EXPECT_EQ(albedo->sourceTarget(), rt.get());
    EXPECT_EQ(normal->sourceTarget(), rt.get());
    EXPECT_EQ(backend->program_draws, before + 2);
    EXPECT_EQ(backend->last_program_source, rt.get());
    EXPECT_EQ(backend->last_program, normal_program.get());   // the later pass read attachment 1
}

TEST(RenderEngineTest, ScreenPassProgramSamplesMrtForDeferredLighting)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
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
    producer->setCamera(cam);
    producer->setRenderTarget(rt);
    producer->setOutputName(u8"GBuffer");
    engine->addPass(producer, -3);

    // Lighting consumer: a ScreenPass carrying a fragment program samples the
    // whole MRT source (all attachments) through the backend.
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    ShaderStage fs;
    fs.type = ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec2 vine_uv;\n"
                u8"layout(location = 0) out vec4 out_color;\n"
                u8"void main() { out_color = vec4(vine_uv, 0.0, 1.0); }\n";
    program->addStage(fs);

    auto light = intrusive_ptr<ScreenPass>(new ScreenPass());
    light->addInputName(u8"GBuffer");
    light->setCamera(cam);
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
    const vn::math::Vec3d dir(0.2, -0.5, -0.8);
    auto light = Light::createDirectional(dir);
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->type(), LightType::Directional);
    EXPECT_TRUE(light->hasDirection());
    EXPECT_EQ(light->direction(), dir);

    light->setDirection(vn::math::Vec3d(1.0, 0.0, 0.0));
    EXPECT_EQ(light->direction(), vn::math::Vec3d(1.0, 0.0, 0.0));
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
    auto sun     = Light::createDirectional(vn::math::Vec3d(0.0, -1.0, 0.0));
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
    pass->setCamera(camera);
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
    auto sun = Light::createDirectional(vn::math::Vec3d(0.0, -1.0, 0.0));
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
    engine->setDevice(backend);
    engine->initialize();

    auto content = intrusive_ptr<Scene>(new Scene());
    content->setRoot(makeTriangleNode(Vec3d(0.0, 0.0, 0.0), nullptr));

    auto cam = intrusive_ptr<Camera>(new Camera());
    auto main = intrusive_ptr<RenderPass>(new RenderPass());
    main->setCamera(cam);
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
    shadow->setCamera(light_cam);
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
    engine->setDevice(backend);
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
        Viewport{ 8, 8, 320, 180 });
    ASSERT_NE(screen, nullptr);
    // The recipe adds the off-screen (order < 0) + screen (order > 0) passes.
    EXPECT_EQ(engine->passCount(), 2u);

    // The recipe NAMES the copy program on the pass it just built (the SDK's own recipes do not get
    // an implicit copy either — they pin the SDK program instead).
    EXPECT_NE(screen->program(), nullptr);
    EXPECT_EQ(screen->program()->name(), vn::graphics::screenCopyProgram()->name());

    const int draws_before = backend->program_draws;
    engine->frame();
    // The screen pass drew the published off-screen target exactly once, through that program.
    EXPECT_EQ(backend->program_draws - draws_before, 1);
    EXPECT_EQ(screen->sourceTarget(), engine->resolve(u8"SceneColor"));
    ASSERT_NE(engine->resolve(u8"SceneColor"), nullptr);
    EXPECT_TRUE(engine->resolve(u8"SceneColor")->hasColor());
}

TEST(RenderPipelineBuilderTest, ForwardPathBuildsWindowPass)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(PipelineOptions{});

    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(pipeline->windowPass(), nullptr);
    EXPECT_EQ(engine->passCount(), 1u);
    // One pass, and the plan says what it is: the window pass drawing the bound content through the pipeline's
    // camera to the backbuffer, clearing the window grey - the frame's shape in one line instead of three
    // assertions about a handle.
    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 1u);
    EXPECT_EQ(plan.passes[0].pass, pipeline->windowPass());
    EXPECT_EQ(plan.passes[0].name, u8"main");
    EXPECT_EQ(plan.passes[0].content, content.get());
    EXPECT_EQ(plan.passes[0].camera, cam.get());
    EXPECT_EQ(plan.passes[0].target, nullptr) << "no target: the window";
    EXPECT_TRUE(plan.passes[0].clears);
    EXPECT_EQ(engine->contentOf(pipeline->windowPass()), content.get());
    EXPECT_EQ(pipeline->offscreenTarget(), nullptr);
}

/**
 * @brief A pipeline every pass of which is placed by a STAGE, not by a hand-picked number.
 *
 * The stage table is what lets an effect enter relative to the passes it serves; it is only worth
 * anything if the assembled passes actually land in the order the stages state.
 */
TEST(RenderPipelineBuilderTest, StagesOrderThePasses)
{
    EXPECT_LT(pipelineStageOrder(PipelineStage::Depth), pipelineStageOrder(PipelineStage::Geometry));
    EXPECT_LT(pipelineStageOrder(PipelineStage::Geometry), pipelineStageOrder(PipelineStage::Effect));
    EXPECT_LT(pipelineStageOrder(PipelineStage::Effect), pipelineStageOrder(PipelineStage::Shading));
    EXPECT_LT(pipelineStageOrder(PipelineStage::Shading), pipelineStageOrder(PipelineStage::Transparent));
    EXPECT_LT(pipelineStageOrder(PipelineStage::Transparent), pipelineStageOrder(PipelineStage::Present));
    EXPECT_LT(pipelineStageOrder(PipelineStage::Present), pipelineStageOrder(PipelineStage::Overlay));
    EXPECT_LT(pipelineStageOrder(PipelineStage::Overlay), pipelineStageOrder(PipelineStage::Preview));
}

/**
 * @brief Dropping the pipeline handle takes its passes out of the frame.
 *
 * The engine keeps its own reference to every registered pass, so a handle that only held
 * references would leave its passes running with nobody able to name them (the defect the
 * pipeline's ownership exists to close). Removing the passes is the handle's job, and the count
 * going back to zero is what says it happened.
 */
TEST(RenderPipelineBuilderTest, DroppingThePipelineUnregistersItsPasses)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    {
        PipelineOptions opts;
        opts.path = ShadingPath::Deferred;
        opts.offscreen_width  = 64;
        opts.offscreen_height = 64;
        auto pipeline         = builder.build(opts);
        ASSERT_NE(pipeline, nullptr);
        EXPECT_EQ(engine->passCount(), 2u); // G-buffer + lighting
        EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), cam.get()));
    }
    EXPECT_EQ(engine->passCount(), 0u) << "a dropped pipeline must not leave its passes registered";
}

TEST(RenderPipelineBuilderTest, AShadowThisPipelineCannotShadeIsReported)
{
    // A shadow is asked for by the LIGHT that casts it (Light::castShadow) rather than by a preset
    // name, because the light outlives any one pipeline and a scene may be drawn by several. Both
    // paths BUILD that shadow (the same pass, one implementation) — what a pipeline still cannot do
    // is shade with a map it was never given a program for, and THAT is what is reported: a host that
    // supplied its own deferred lighting program gets no shadow pass (the shading is theirs), and
    // saying so is the difference between a picture they asked for and one that quietly differs.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    std::vector<RenderDiagnostic> reported;
    engine->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);

    // Nothing asked for: nothing to report.
    ASSERT_NE(builder.build(PipelineOptions{}), nullptr);
    EXPECT_TRUE(reported.empty());

    content->addLight(Light::createDirectional());
    ASSERT_NE(builder.build(PipelineOptions{}), nullptr);
    EXPECT_TRUE(reported.empty()) << "a light that casts no shadow asks for nothing";

    content->lights().front()->setCastShadow(true);
    auto forward = builder.build(PipelineOptions{});
    ASSERT_NE(forward, nullptr);
    // The FORWARD path HONOURS the request (S2b): the shadow pass is built and the content pass
    // declares its map as an input, so there is nothing to report. Whether that shadow then reaches
    // the picture is decided by the content PROGRAM — the set declares the ABI and the program has to
    // use it — which is a per-drawable question the backend answers (see the vsg backend's
    // "declares no shadow_map" report).
    EXPECT_TRUE(reported.empty()) << "the forward path builds the shadow pass, so it has nothing to report";
    EXPECT_NE(forward->windowPass(), nullptr);
    EXPECT_EQ(forward->offscreenTarget(), nullptr);

    // The SAME request on the deferred path is honoured the same way: the pass is built, so there is
    // nothing to report. A report here — which the unconditional report this replaced produced — tells
    // the host its picture is unshadowed while it is shadowed, i.e. the one thing a diagnostic must
    // not do. (vsg_backend_selftest's deferred shadow phase is what caught it: the map was bound and
    // empty while the host had been told the shadow was not built at all.)
    reported.clear();
    PipelineOptions opts;
    opts.path             = ShadingPath::Deferred;
    opts.offscreen_width  = 64;
    opts.offscreen_height = 64;
    auto deferred         = builder.build(opts);
    ASSERT_NE(deferred, nullptr);
    EXPECT_TRUE(reported.empty()) << "the deferred path built the shadow, so it has nothing to report";
    EXPECT_NE(deferred->offscreenTarget(), nullptr);

    // A host-supplied lighting program is the one case left: the shading is theirs, so the shadow map
    // they would not read is not built, and that IS reported.
    reported.clear();
    opts.lighting_program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    ASSERT_NE(builder.build(opts), nullptr);
    ASSERT_EQ(reported.size(), 1u);
    EXPECT_EQ(reported[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(reported[0].category, DiagnosticCategory::UnsupportedRequest);
    EXPECT_NE(reported[0].message.find(u8"no shadow pass"), vn::String::npos);

    // A disabled light asks for nothing, even with the flag still set.
    reported.clear();
    content->lights().front()->setEnabled(false);
    ASSERT_NE(builder.build(PipelineOptions{}), nullptr);
    EXPECT_TRUE(reported.empty());
}

/**
 * @brief The deferred path BUILDS the shadow a castShadow light asks for.
 *
 * The pixel proof lives in vsg_backend_selftest (only a GPU can tell a shadow from a dark pixel),
 * so this one pins the STRUCTURE the pixels then depend on: the frame opens with a pass drawing
 * into a depth-only target at the light's resolution, whose depth stays sampleable, which states
 * the SDK's ONE light-camera derivation for that light and that content, and which is drawn
 * through an ORTHOGRAPHIC camera that is not the view camera. A shadow pass that renders through
 * the view camera, or that leaves its matrix to the identity, looks exactly like this one until
 * the picture is examined.
 *
 * It also pins that the lighting pass' declared input is RESOLVED (the engine reports a pass that
 * reads what no pass produced), which is the wire the map travels on.
 */
TEST(RenderPipelineBuilderTest, ARequestedShadowIsBuiltOnTheDeferredPath)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->initialize();

    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0));
    auto sun = LightPtr(Light::createDirectional(Vec3d(0.0, -1.0, 0.0)));
    sun->setCastShadow(true);
    sun->setShadowResolution(64);
    content->addLight(sun);

    PipelineOptions opts;
    opts.path             = ShadingPath::Deferred;
    opts.offscreen_width  = 32;
    opts.offscreen_height = 32;
    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    // The handle is HELD: dropping it unregisters its passes (Pipeline's RAII), so a test that
    // measured a dropped pipeline would measure an empty frame.
    auto pipeline = builder.build(opts);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_EQ(engine->passCount(), 3u) << "shadow + G-buffer + lighting (which IS the window pass "
                                          "while nothing is composited over it)";

    backend->target_history.clear();
    backend->render_cameras.clear();
    const std::size_t problems_before = engine->diagnosticCount();
    engine->frame();

    ASSERT_GE(backend->target_history.size(), 2u);
    RenderTarget* shadow = backend->target_history.front();
    ASSERT_NE(shadow, nullptr);
    EXPECT_TRUE(shadow->hasDepth());
    EXPECT_FALSE(shadow->hasColor()) << "a shadow map carries depth, not a picture";
    EXPECT_EQ(shadow->width(), 64) << "the map is the side length the light asked for";
    EXPECT_EQ(shadow->height(), 64);
    EXPECT_TRUE(shadow->depthPromotion()) << "the lighting samples this depth, so the pass may not preserve it";

    // The stated matrix IS the one derivation, for this light and this content.
    auto         light_camera = intrusive_ptr<Camera>(new Camera());
    const Mat4d  expected     = RenderPipelineBuilder::directionalShadowMatrix(*sun, content->boundingBox(), *light_camera);
    const Mat4d  stated       = shadow->producerViewProjection();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_DOUBLE_EQ(stated(r, c), expected(r, c)) << "at (" << r << ", " << c << ")";
        }
    }

    // ...and the pass drew the content through an orthographic camera that is not the view camera.
    ASSERT_FALSE(backend->render_cameras.empty());
    const Camera* shadow_camera = backend->render_cameras.front();
    ASSERT_NE(shadow_camera, nullptr);
    EXPECT_NE(shadow_camera, cam.get()) << "the shadow pass looks along the light, not through the view";
    EXPECT_EQ(shadow_camera->projectionType(), Camera::ProjectionType::Orthographic);

    // The map reached the lighting: a declared input nothing produced is reported by the engine.
    EXPECT_EQ(engine->diagnosticCount(), problems_before);
}

/**
 * @brief The forward path builds the SAME shadow, and hands it to its content pass.
 *
 * The structural sibling of the deferred test above, and the reason it exists separately is the
 * hand-off: a forward pipeline has no G-buffer and no fullscreen program, so the map travels as a
 * declared INPUT of the content pass itself (the pass that draws into the window), which is what
 * makes the backend bind it into the content set's shadow binding. A forward path that built the
 * pass but forgot the declaration would look complete here and shade an unshadowed picture on the
 * GPU - the one failure the pixel gate in vsg_backend_selftest exists to catch.
 */
TEST(RenderPipelineBuilderTest, ARequestedShadowIsBuiltOnTheForwardPathToo)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->initialize();

    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0));
    auto sun = LightPtr(Light::createDirectional(Vec3d(0.0, -1.0, 0.0)));
    sun->setCastShadow(true);
    sun->setShadowResolution(64);
    content->addLight(sun);

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(PipelineOptions{});
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 2u) << "shadow + the content pass (which IS the window pass)";

    backend->target_history.clear();
    backend->render_cameras.clear();
    const std::size_t problems_before = engine->diagnosticCount();
    engine->frame();

    // The frame OPENS with the shadow: it must be recorded before the pass that samples it.
    ASSERT_GE(backend->target_history.size(), 2u);
    RenderTarget* shadow = backend->target_history.front();
    ASSERT_NE(shadow, nullptr);
    EXPECT_TRUE(shadow->hasDepth());
    EXPECT_FALSE(shadow->hasColor());
    EXPECT_EQ(shadow->width(), 64);
    EXPECT_TRUE(shadow->depthPromotion());
    EXPECT_EQ(backend->target_history[1], nullptr) << "the content pass presents to the window";

    // The one derivation, and an orthographic camera that is not the view camera (see the deferred
    // test: the same assertions, because it is the same pass builder).
    auto        light_camera = intrusive_ptr<Camera>(new Camera());
    const Mat4d expected     = RenderPipelineBuilder::directionalShadowMatrix(*sun, content->boundingBox(), *light_camera);
    const Mat4d stated       = shadow->producerViewProjection();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_DOUBLE_EQ(stated(r, c), expected(r, c)) << "at (" << r << ", " << c << ")";
        }
    }
    ASSERT_FALSE(backend->render_cameras.empty());
    ASSERT_NE(backend->render_cameras.front(), nullptr);
    EXPECT_NE(backend->render_cameras.front(), cam.get());
    EXPECT_EQ(backend->render_cameras.front()->projectionType(), Camera::ProjectionType::Orthographic);

    // The hand-off: the content pass no longer declares an input nothing produced. Without that
    // declaration the engine reports it here, which is exactly the failure this test is about.
    EXPECT_EQ(engine->diagnosticCount(), problems_before);
}

TEST(RenderPipelineBuilderTest, DeferredPresetBuildsGbufferAndLightingPasses)
{
    auto engine     = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content    = intrusive_ptr<Scene>(new Scene());
    auto cam        = intrusive_ptr<Camera>(new Camera());
    auto gbuf_prog  = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    auto light_prog = intrusive_ptr<ShaderProgram>(new ShaderProgram());

    PipelineOptions opts;
    opts.path = ShadingPath::Deferred;
    opts.offscreen_width   = 640;
    opts.offscreen_height  = 360;
    opts.gbuffer_program   = gbuf_prog;
    opts.lighting_program  = light_prog;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(opts);

    ASSERT_NE(pipeline, nullptr);
    // G-buffer (order < 0) + fullscreen lighting window pass (order 0).
    EXPECT_EQ(engine->passCount(), 2u);
    // The lighting pass presents the view camera, so a host that declares it owns the surface's presentation
    // (RenderSurface) - no second forward window pass beside it.
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), cam.get()));

    RenderPass* window = pipeline->windowPass();
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->camera(), cam.get());
    EXPECT_EQ(window->renderTarget(), nullptr);

    // The G-buffer pass draws the content THROUGH the G-buffer program, and it is a SCENE pass: the program
    // rides on that pass' OVERRIDE. `PassDesc::program` is the field only a Screen pass draws through, so a
    // scene declaration that names it is a scene pass with NO program at all - every body then draws with its
    // own program (or the engine's forward one) which writes ONE colour output: the geometry lands in albedo
    // and the normal / specular / position attachments keep their transparent-black clear, which is exactly
    // what made the demo's three G-buffer previews black (see SdkPixelTest's deferred case for the pixels).
    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 2U);
    const RenderPass* const geometry_pass = plan.passes.front().pass;
    ASSERT_NE(geometry_pass, nullptr);
    EXPECT_EQ(plan.passes.front().order, -40) << "几何 pass 在光照之前";
    EXPECT_EQ(geometry_pass->programOverride(), gbuf_prog.get()) << "G-buffer 那条 pass 必须带着 MRT 的几何程序";
    EXPECT_EQ(window->programOverride(), nullptr) << "lighting 是 fullscreen pass：程序在它自己的 program 上";
    EXPECT_EQ(static_cast<ScreenPass*>(window)->program(), light_prog.get());

    RenderTarget* gbuffer = pipeline->offscreenTarget();
    ASSERT_NE(gbuffer, nullptr);
    EXPECT_EQ(gbuffer->width(), 640);
    EXPECT_EQ(gbuffer->height(), 360);
    EXPECT_TRUE(gbuffer->hasColor());
    EXPECT_TRUE(gbuffer->hasDepth());
    EXPECT_EQ(gbuffer->colorCount(), 4) << "延迟预设的 G-buffer 是四张彩色附件（见 defaultGbufferTarget）";

    // Creator-maintained sizing: the handle resizes the G-buffer target (a ratio of 1 keeps the surface
    // size and the target size the same number; the space split itself is pinned by
    // ResizeMapsOneSurfaceSizeIntoTheSpaceEachConsumerNeeds).
    pipeline->resize(1280, 720, 1.0);
    EXPECT_EQ(gbuffer->width(), 1280);
    EXPECT_EQ(gbuffer->height(), 720);
}

/**
 * @brief The light camera frames the content, and there is exactly one derivation of it.
 *
 * The shadow pass renders through this camera and the target STATES its view-projection, so the two
 * cannot disagree about where the light was — which is why the function is public rather than a
 * private helper the shadow pass happens to share: a host (or a test) that builds its own shadow
 * pass has to get the SAME camera, and re-deriving one is the failure mode this pins.
 */
TEST(RenderPipelineBuilderTest, TheDirectionalShadowCameraFramesTheContentFromTheLightSide)
{
    auto sun = LightPtr(Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0)));
    sun->setName(u8"sun");

    // A box that is not at the origin: the window has to follow the content, not the world.
    const vn::math::Rect3<double> bounds(vn::math::Point3<double>(4.0, 2.0, 0.0),
                                          vn::math::Point3<double>(6.0, 4.0, 2.0));

    auto camera = intrusive_ptr<Camera>(new Camera());
    const vn::math::Mat4d view_projection = RenderPipelineBuilder::directionalShadowMatrix(*sun, bounds, *camera);

    // Every corner of the box lands inside the clip volume (the map has to cover the casters), and it
    // is the ORTHO window that puts it there — the shading maps fragments through this same matrix.
    for (const auto& corner : { vn::math::Point3<double>(4.0, 2.0, 0.0), vn::math::Point3<double>(4.0, 4.0, 2.0),
                                vn::math::Point3<double>(6.0, 2.0, 2.0), vn::math::Point3<double>(6.0, 4.0, 0.0) }) {
        const vn::math::Point3<double> ndc = view_projection * corner;
        EXPECT_GE(ndc.x, -1.0);
        EXPECT_LE(ndc.x, 1.0);
        EXPECT_GE(ndc.y, -1.0);
        EXPECT_LE(ndc.y, 1.0);
        EXPECT_GE(ndc.z, -1.0) << "the content sits inside the depth range, not clipped away";
        EXPECT_LE(ndc.z, 1.0);
    }
    // ...and the eye sits on the side the light comes FROM (the light shines along its direction).
    const vn::math::Vec3d eye = camera->eye();
    EXPECT_NEAR(eye.x, 5.0, 1e-9) << "the eye is centred on the content, so the window is symmetric";
    EXPECT_NEAR(eye.y, 3.0, 1e-9);
    EXPECT_GT(eye.z, 2.0) << "with a -z sun the light camera looks from +z";
}

/**
 * @brief A pass' declared inputs reach the backend, resolved and in declaration order.
 *
 * This is the wire an effect reads what an earlier pass wrote through (a shadow map, a screen-space
 * occlusion buffer): the pass states WHAT it reads and the engine hands the resolved targets to the
 * backend, which binds them where its shader's ABI says. A pass that declares nothing announces an
 * empty list rather than skipping the call, so a backend can never keep a stale one from the pass
 * before.
 */
TEST(RenderPipelineBuilderTest, DeclaredPassInputsReachTheBackend)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize()) << "a frame runs nothing until the engine is initialized";

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    ASSERT_NE(builder.build(PipelineOptions{}), nullptr);

    // The G-buffer of a deferred pipeline is a target a later pass can declare; here it stands in
    // for any producer's output.
    auto produced = intrusive_ptr<RenderTarget>(new RenderTarget());
    produced->setName(u8"Produced");
    produced->setSize(64, 64);
    engine->publish(u8"Produced", produced);

    auto consumer = intrusive_ptr<RenderPass>(new RenderPass());
    consumer->setCamera(cam);
    consumer->addInputName(u8"Produced");
    consumer->addInputName(u8"NothingPublishesThis");
    engine->addPass(consumer, 1);

    engine->frame(0.016);

    ASSERT_GE(backend->pass_input_sets, 1);
    ASSERT_NE(backend->last_pass_inputs.size(), 0u) << "the last pass declared two inputs";
    // Resolved in declaration order, null where nothing produced them: a pass learns what is
    // missing from the same list it reads what is there.
    ASSERT_EQ(backend->last_pass_inputs.size(), 2u);
    EXPECT_EQ(backend->last_pass_inputs[0], produced.get());
    EXPECT_EQ(backend->last_pass_inputs[1], nullptr);
}

TEST(RenderPipelineBuilderTest, DeferredPresetFallsBackToBuiltinPrograms)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    // No programs supplied: the builder supplies its built-in temporary
    // G-buffer geometry + deferred-lighting programs.
    PipelineOptions opts;
    opts.path = ShadingPath::Deferred;
    opts.offscreen_width  = 640;
    opts.offscreen_height = 360;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(opts);

    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), cam.get()));
    // The lighting window pass received the built-in default fragment program.
    auto* light = dynamic_cast<ScreenPass*>(pipeline->windowPass());
    ASSERT_NE(light, nullptr);
    EXPECT_NE(light->program(), nullptr);
}

TEST(RenderPipelineBuilderTest, DeferredPathRefusesWithoutContent)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto cam    = intrusive_ptr<Camera>(new Camera());

    // A Deferred main pass needs a content scene to fill its G-buffer.
    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    PipelineOptions opts;
    opts.path     = ShadingPath::Deferred;
    auto pipeline = builder.build(opts);

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
    opts.path = ShadingPath::Deferred;
    opts.offscreen_width  = 640;
    opts.offscreen_height = 360;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    builder.setTransparentContent(overlay);
    auto pipeline = builder.build(opts);

    ASSERT_NE(pipeline, nullptr);
    // G-buffer + lighting INTO the composite + forward transparent + present:
    // four registered passes (the opaque scene is rasterised once — the
    // composite reuses the G-buffer depth instead of re-rendering it).
    EXPECT_EQ(engine->passCount(), 4u);
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), cam.get()));

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
    EXPECT_FALSE(present->isClearEnabled());
    const auto& inputs = present->inputNames();
    EXPECT_NE(std::find(inputs.begin(), inputs.end(), vn::String(u8"Composite")), inputs.end());

    // Sizing is creator-maintained for both off-screen targets together.
    pipeline->resize(1280, 720, 1.0);
    EXPECT_EQ(pipeline->offscreenTarget()->width(), 1280);
    EXPECT_EQ(pipeline->compositeTarget()->width(), 1280);
    EXPECT_EQ(pipeline->offscreenTarget()->height(), 720);
    EXPECT_EQ(pipeline->compositeTarget()->height(), 720);
}

/**
 * @brief What a recipe writes down is what the plan says: the tags, and the stages that place the passes.
 *
 * The builder's ordinary passes are DECLARATIONS now (see PassDesc), so this pins the part a reader of a plan
 * depends on: every row is labelled by the tag the recipe gave it, and the order of the rows is the order the
 * declared STAGES imply rather than a number the builder picked out of the air.
 */
TEST(RenderPipelineBuilderTest, TheRecipeIsAWrittenDeclarationAndThePlanSaysSo)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto overlay = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    PipelineOptions opts;
    opts.path             = ShadingPath::Deferred;
    opts.offscreen_width  = 640;
    opts.offscreen_height = 360;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    builder.setTransparentContent(overlay);
    auto pipeline = builder.build(opts);
    ASSERT_NE(pipeline, nullptr);

    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 4u);
    EXPECT_EQ(plan.passes[0].tag, vn::String(u8"GBuffer"));
    EXPECT_EQ(plan.passes[1].tag, vn::String(u8"Lighting"));
    EXPECT_EQ(plan.passes[2].tag, vn::String(u8"Transparent"));
    EXPECT_EQ(plan.passes[3].tag, vn::String(u8"Present"));
    // The stage is what ordered them: the numbers below are the stage orders, not choices of the recipe's.
    EXPECT_EQ(plan.passes[0].order, pipelineStageOrder(PipelineStage::Geometry));
    EXPECT_EQ(plan.passes[1].order, pipelineStageOrder(PipelineStage::Shading));
    EXPECT_EQ(plan.passes[2].order, pipelineStageOrder(PipelineStage::Transparent));
    EXPECT_EQ(plan.passes[3].order, pipelineStageOrder(PipelineStage::Present));
    // Every row is still a pass with a name, a camera and a place to draw: a declaration did not cost the plan
    // anything (see the deferred preset test above for what the passes themselves are).
    for (const PassPlan& row : plan.passes) {
        EXPECT_FALSE(row.name.empty());
        EXPECT_EQ(row.camera, cam.get());
    }
}

/**
 * @brief The HUD overlays are DECLARATIONS too: what SHOWS them is a pass, what they ARE is content.
 *
 * The axis gizmo and the frame-rate readout used to be RenderPass subclasses whose execute() drew their own scene
 * and ignored the one the engine handed them. They are the two motivating users of PassDesc::before_draw, so this
 * pins the whole shape: the pass carries the overlay's own camera and content, a HUD tag, the HUD stage's order
 * and no clear; the per-frame work is the declaration's hook, and it runs when the FRAME runs.
 */
TEST(RenderPipelineBuilderTest, TheHudOverlaysAreDeclarationsToo)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    PipelineOptions opts;
    opts.path                = ShadingPath::Forward;
    opts.gizmo.source_camera = cam.get();
    opts.gizmo.box_size      = 96;
    opts.fps.enabled         = true;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(opts);
    ASSERT_NE(pipeline, nullptr);

    // Two handles per overlay, and they answer different questions (see Pipeline::gizmo/gizmoPass).
    AxisGizmo* const  gizmo        = pipeline->gizmo();
    FpsOverlay* const readout      = pipeline->fpsOverlay();
    RenderPass* const gizmo_pass   = pipeline->gizmoPass();
    RenderPass* const readout_pass = pipeline->fpsOverlayPass();
    ASSERT_NE(gizmo, nullptr);
    ASSERT_NE(readout, nullptr);
    ASSERT_NE(gizmo_pass, nullptr);
    ASSERT_NE(readout_pass, nullptr);

    const FramePlan plan      = engine->framePlan();
    const PassPlan* gizmo_row = nullptr;
    const PassPlan* readout_row = nullptr;
    for (const PassPlan& row : plan.passes) {
        if (row.name == vn::String(u8"axis_gizmo")) {
            gizmo_row = &row;
        }
        if (row.name == vn::String(u8"fps_overlay")) {
            readout_row = &row;
        }
    }
    ASSERT_NE(gizmo_row, nullptr);
    ASSERT_NE(readout_row, nullptr);

    // The declaration says what the pass IS: the overlay's own camera and scene (it used to draw them behind the
    // engine's back), a HUD tag, the HUD stage's order, and no clear.
    EXPECT_EQ(gizmo_row->tag, vn::String(u8"Hud"));
    EXPECT_EQ(readout_row->tag, vn::String(u8"Hud"));
    EXPECT_EQ(gizmo_row->order, pipelineStageOrder(PipelineStage::Overlay) + opts.gizmo.order);
    EXPECT_EQ(readout_row->order, pipelineStageOrder(PipelineStage::Overlay) + opts.fps.order);
    EXPECT_FALSE(gizmo_row->clears) << "a HUD draws over the picture it is on, it never washes it out";
    EXPECT_EQ(gizmo_row->camera, gizmo->camera());
    EXPECT_EQ(gizmo_row->content, gizmo->content());
    EXPECT_EQ(readout_row->camera, readout->camera());
    EXPECT_EQ(readout_row->content, readout->content());
    EXPECT_EQ(gizmo_pass->depthMode(), DepthMode::Disabled) << "a HUD is always on top";

    // And the per-frame hook is LIVE: the mirror happens because the FRAME runs (PassDesc::before_draw), not
    // because somebody remembered to call update().
    cam->setViewMatrixAsLookAt(vn::math::Vec3d(2.0, 3.0, 6.0), vn::math::Vec3d(0.0, 0.0, 0.0),
                               vn::math::Vec3d(0.0, 1.0, 0.0));
    engine->frame(0.016);

    const Vec3d source_forward = (cam->target() - cam->eye()).normalized();
    const Vec3d shown_forward  = (gizmo->camera()->target() - gizmo->camera()->eye()).normalized();
    EXPECT_NEAR(shown_forward.x, source_forward.x, 1.0e-6);
    EXPECT_NEAR(shown_forward.y, source_forward.y, 1.0e-6);
    EXPECT_NEAR(shown_forward.z, source_forward.z, 1.0e-6);
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
    auto pipeline = builder.build(PipelineOptions{});

    ASSERT_NE(pipeline, nullptr);
    // Main order-0 window content pass + order-1 transparent content pass.
    EXPECT_EQ(engine->passCount(), 2u);
    ASSERT_NE(pipeline->windowPass(), nullptr);
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), cam.get()));
    EXPECT_EQ(engine->contentOf(pipeline->windowPass()), content.get());
    // Forward composites nothing off-screen: no composite target.
    EXPECT_EQ(pipeline->compositeTarget(), nullptr);
}

TEST(RenderPassTest, DepthStyleIsExplicitAndIndependentOfClear)
{
    RenderPass pass;
    // Depth style is explicit and defaults on; it is NOT inferred from the
    // clear flag (a depth-on pass may skip clearing).
    EXPECT_EQ(pass.depthMode(), DepthMode::TestAndWrite);
    EXPECT_TRUE(pass.isClearEnabled());
    pass.setDepthMode(DepthMode::Disabled);
    EXPECT_EQ(pass.depthMode(), DepthMode::Disabled);
    // The two axes are orthogonal: disabling the clear does not flip depth.
    pass.setClearEnabled(false);
    EXPECT_FALSE(pass.isClearEnabled());
    EXPECT_EQ(pass.depthMode(), DepthMode::Disabled);
    pass.setDepthMode(DepthMode::TestAndWrite);
    EXPECT_EQ(pass.depthMode(), DepthMode::TestAndWrite);
    EXPECT_FALSE(pass.isClearEnabled());
    // All THREE values are reachable, and the middle one stays itself: a test-only pass must not be
    // flattened into "on" by reading a boolean and writing it back (the convenience pair that did
    // exactly that is gone — setDepthMode is the only spelling of this fact).
    pass.setDepthMode(DepthMode::TestOnly);
    EXPECT_EQ(pass.depthMode(), DepthMode::TestOnly);
}

TEST(RenderPipelineBuilderTest, DefaultGbufferTargetIsCanonicalLayout)
{
    auto gbuffer = RenderPipelineBuilder::defaultGbufferTarget(320, 180);
    ASSERT_NE(gbuffer, nullptr);
    EXPECT_EQ(gbuffer->width(), 320);
    EXPECT_EQ(gbuffer->height(), 180);
    EXPECT_EQ(gbuffer->colorCount(), 4);  // albedo / normal+shininess / spec / pos
    EXPECT_TRUE(gbuffer->hasDepth());
    // THE FORMATS ARE PART OF THE LAYOUT, and the albedo's is a measured choice (D5, 2026-09-25): the 8-bit
    // store's step reaches the eye multiplied by the light, so it is invisible under the demo's dim lighting
    // and clearly visible under a bright one (2 LSB of error / 16 levels against 1 LSB / 62 levels for the
    // half-float store - see ContentPassTest's `MeasureWhatAHalfFloatAlbedoWouldBuyEndToEnd`).
    EXPECT_EQ(gbuffer->colorFormat(0), RenderTarget::ColorFormat::RGBA16F);  // albedo
    EXPECT_EQ(gbuffer->colorFormat(1), RenderTarget::ColorFormat::RGBA16F);  // view normal + shininess
    EXPECT_EQ(gbuffer->colorFormat(2), RenderTarget::ColorFormat::RGBA8);    // specular colour
    EXPECT_EQ(gbuffer->colorFormat(3), RenderTarget::ColorFormat::RGBA16F);  // view position

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
    engine->setDevice(backend);
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
    auto pipeline = builder.build(opts);

    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(pipeline->gizmo(), nullptr);
    // Window pass + the gizmo HUD pass.
    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_TRUE(drawsToWindow(pipeline->windowPass(), cam.get()));

    // resize() re-anchors the gizmo even without an off-screen target; a
    // frame draws both the window and the gizmo without crashing.
    pipeline->resize(1600, 900, 2.0);
    engine->frame();
}

TEST(RenderPipelineBuilderTest, ResizeMapsOneSurfaceSizeIntoTheSpaceEachConsumerNeeds)
{
    auto engine     = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content    = intrusive_ptr<Scene>(new Scene());
    auto cam        = intrusive_ptr<Camera>(new Camera());
    auto gbuf_prog  = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    auto light_prog = intrusive_ptr<ShaderProgram>(new ShaderProgram());

    PipelineOptions opts;
    opts.path                = ShadingPath::Deferred;
    opts.offscreen_width     = 640;
    opts.offscreen_height    = 360;
    opts.gbuffer_program     = gbuf_prog;
    opts.lighting_program    = light_prog;
    opts.gizmo.source_camera = cam.get();
    opts.gizmo.pixel_ratio   = 2.0;
    opts.gizmo.box_size      = 128;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(opts);
    ASSERT_NE(pipeline, nullptr);
    RenderTarget* gbuffer = pipeline->offscreenTarget();
    ASSERT_NE(gbuffer, nullptr);
    AxisGizmo* gizmo = pipeline->gizmo();
    ASSERT_NE(gizmo, nullptr);
    // The overlay's BOX is stated on the pass that draws it (a declaration, see PassDesc): the content object
    // computes it from the surface size, the pass states it, and this is where a reader sees what the frame does.
    RenderPass* const gizmo_pass = pipeline->gizmoPass();
    ASSERT_NE(gizmo_pass, nullptr);

    // ONE call states the surface size the host HAS (logical - the number Qt reports and the number a
    // SceneView layout step hands over) plus the ratio, and the handle gives each consumer the space it
    // reads: the G-buffer is DEVICE sized (the light pass samples it 1:1 with the swapchain), while the
    // gizmo is laid out on the LOGICAL surface and applies the ratio itself.
    //
    // Both halves were wrong in opposite directions before: the signature offered no ratio, so a caller had
    // to pre-scale for the target and then re-call the HUD passes with the logical size to undo the damage
    // (which is what the app shell did). The old fixtures all ran at ratio 1.0, where the two spaces
    // coincide - so neither half was visible.
    pipeline->resize(800, 600, 2.0);

    EXPECT_EQ(gbuffer->width(), 1600);
    EXPECT_EQ(gbuffer->height(), 1200);

    ASSERT_TRUE(gizmo_pass->hasViewport());
    const Viewport gizmo_rect = gizmo_pass->viewport();
    EXPECT_EQ(gizmo_rect.width, 128) << "the box is stated in device pixels and must not be scaled twice";
    EXPECT_EQ(gizmo_rect.x, 16);
    EXPECT_EQ(gizmo_rect.y, 600 * 2 - 16 - 128) << "anchored on the LOGICAL surface (600), ratio applied once";
}

// ============ HUD passes (top / overlay content) ============

TEST(HudPassTest, EngineDrawsPassesInOrderAndSkipsDisabled)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->initialize();

    // A clearing window pass (explicit content scene) below the HUD passes, as
    // a real viewer would register; HUD passes draw on top.
    auto win_content = intrusive_ptr<Scene>(new Scene());
    auto master_cam = intrusive_ptr<Camera>(new Camera());
    auto window = intrusive_ptr<RenderPass>(new RenderPass());
    window->setCamera(master_cam);
    engine->addPass(window, win_content, 0);

    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    auto scene = intrusive_ptr<Scene>(new Scene());

    // Two enabled HUD passes at ascending orders plus a disabled one; every
    // HUD pass draws over the previous content (clearing disabled).
    auto low = intrusive_ptr<RenderPass>(new RenderPass());
    low->setCamera(cam);
    low->setClearEnabled(false);
    engine->addPass(low, scene, 5);

    auto high = intrusive_ptr<RenderPass>(new RenderPass());
    high->setCamera(cam);
    high->setClearEnabled(false);
    engine->addPass(high, scene, 10);

    auto hidden = intrusive_ptr<RenderPass>(new RenderPass());
    hidden->setCamera(cam);
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
    engine->setDevice(backend);
    engine->initialize();

    // A clearing window pass below the HUD pass (as a real viewer registers).
    auto win_content = intrusive_ptr<Scene>(new Scene());
    auto master_cam = intrusive_ptr<Camera>(new Camera());
    auto window = intrusive_ptr<RenderPass>(new RenderPass());
    window->setCamera(master_cam);
    engine->addPass(window, win_content, 0);

    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    auto scene = intrusive_ptr<Scene>(new Scene());

    auto hud = intrusive_ptr<RenderPass>(new RenderPass());
    hud->setCamera(cam);
    // A top pass never clears the surface it draws over.
    hud->setClearEnabled(false);
    EXPECT_FALSE(hud->isClearEnabled());
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

TEST(FpsOverlayTest, TheReadoutIsASingleCommandAndBlankUntilItHasAValue)
{
    // The readout is ONE geometry with ONE material, not one node per bar: a bar a digit does not light
    // is written as a bar of no area (see FpsOverlay::writePattern), so the collector sees a single
    // command whatever the digits are -- where seven nodes per digit were 7 to 21 draw commands, and
    // hiding the off ones was the only way to keep a dark "8" out of the picture. What this pins is that
    // the whole readout is one draw, that every bar is always present as vertices, and that it stays out
    // of the frame until it has a value.
    auto overlay = intrusive_ptr<FpsOverlay>(new FpsOverlay());
    Camera camera;
    setupLookAtCamera(camera);

    // No measurement yet, no readout: a blank row is not collected at all (rather than collected as a
    // dark three-eights behind the number).
    EXPECT_TRUE(commandsOf(overlay->content()->root(), camera).empty());

    // Two executes: the first only arms the clock (a frame rate needs two samples), the second feeds the
    // delta to the sampler. A figure is published only when a sampling WINDOW closes (0.5 s by default), so
    // the test shortens the window instead of sleeping half a second; which value comes out of a wall-clock
    // measurement is not what this asks -- only that a value did.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    overlay->sampler().window_seconds = 0.05;
    overlay->update();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    overlay->update();

    const auto commands = commandsOf(overlay->content()->root(), camera);
    ASSERT_EQ(commands.size(), 1u);
    // Three digits x seven bars x six flat faces of four corners: a change rewrites WHICH of these
    // carry area, never how many exist -- which is what lets the backend serve it as one in-place
    // stream refresh instead of a geometry rebuild.
    EXPECT_EQ(commands[0].geometry->vertexCount(), 21u * 24u);
}

TEST(FpsOverlayTest, TheReadoutCameraIsShapedForTheBoxItDrawsInto)
{
    // The digit row is laid out for its own corner box, and projected by its own camera (see FpsOverlay). The box
    // is FITTED and scaled to the surface, so the shape the readout lands in is only known after a resize - and
    // the camera is given it there, because the shape of a picture is the camera's (see Camera; a pass whose
    // rectangle and camera disagree is a stretched picture, which the engine reports).
    auto overlay = intrusive_ptr<FpsOverlay>(new FpsOverlay());
    overlay->setSize(160, 40);
    overlay->onSurfaceResized(800, 600);

    const Viewport box = overlay->boxViewport();
    ASSERT_GT(box.height, 0);
    EXPECT_DOUBLE_EQ(overlay->camera()->aspectRatio(), static_cast<double>(box.width) / box.height);

    // A surface too narrow for the box scales BOTH sides down, and the shape the camera projects for follows it.
    overlay->onSurfaceResized(100, 600);
    const Viewport fitted = overlay->boxViewport();
    ASSERT_GT(fitted.height, 0);
    EXPECT_NE(fitted.width, box.width) << "the narrow surface really did refit the box";
    EXPECT_DOUBLE_EQ(overlay->camera()->aspectRatio(), static_cast<double>(fitted.width) / fitted.height);
}

TEST(FpsOverlayTest, TheFigureAveragesFramesOverWallTimeAndPublishesAtAboutTwoHertz)
{
    // WHAT THE READOUT MEASURES: frames counted over one window of wall time, published when that window
    // closes. A constant frame time makes the figure exactly 1/dt (frames and elapsed are counted from the
    // same deltas), and ten seconds of 60 fps is twenty figures - not the ~67 the old 0.15 s repaint
    // produced.
    FpsOverlay::Sampler sampler;
    int                publishes = 0;
    for (int frame = 0; frame < 600; ++frame) {
        if (sampler.addFrame(1.0 / 60.0)) {
            ++publishes;
            EXPECT_DOUBLE_EQ(sampler.fps(), 60.0);
        }
    }
    EXPECT_GE(publishes, 19);
    EXPECT_LE(publishes, 21);

    // A clock that did not move is not a frame: it must not even feed the window.
    FpsOverlay::Sampler still;
    EXPECT_FALSE(still.addFrame(0.0));
    EXPECT_DOUBLE_EQ(still.fps(), 0.0);
}

TEST(FpsOverlayTest, TheFigureIsExactlyWhatItsWindowAveraged)
{
    // THE CONTRACT: every published figure IS the average of the interval it covers - frames counted over
    // wall time, with nothing blended in. The readout exists to say what the loop really did, so a figure
    // that no interval ever had is a defect however steady it looks (the old per-frame EMA of 1/dt printed
    // 40 and 33 on a real 60 -> 30 step). This recomputes each window's true average from the same deltas
    // and demands the sampler publish exactly that, on a drag-shaped stream.
    FpsOverlay::Sampler sampler;
    std::uint32_t       state     = 12345u;  // an LCG, so the test says which stream it feeds
    int                 in_window = 0;
    double              elapsed   = 0.0;
    int                 publishes = 0;
    for (int frame = 0; frame < 600; ++frame) {
        state                = state * 1664525u + 1013904223u;
        const double roll    = static_cast<double>((state >> 8) & 0xFFFFu) / 65536.0;
        const double stretch = (roll < 0.75) ? 1.0 : ((roll < 0.95) ? 1.5 : 2.5);
        const double dt      = (1.0 / 60.0) * stretch;
        ++in_window;
        elapsed += dt;
        if (!sampler.addFrame(dt)) {
            continue;
        }
        ++publishes;
        EXPECT_DOUBLE_EQ(sampler.fps(), static_cast<double>(in_window) / elapsed)
            << "the figure is the window's own average, not a filtered version of it";
        in_window = 0;
        elapsed   = 0.0;
    }
    EXPECT_GE(publishes, 20);  // ~two a second over ~12 s of stream
    EXPECT_LE(publishes, 26);
}

TEST(FpsOverlayTest, ARealChangeIsShownWithinOneWindowAndOnlyOneWindowMixesTwoRates)
{
    // A sustained change must not be smeared into values that never happened: 3 s at 60 fps then 3 s at
    // 30 fps publishes the 60s, ONE window that really was part 60 and part 30 (its exact average), then
    // the 30s - where the old EMA printed 40, 33, 31 before it settled.
    FpsOverlay::Sampler sampler;
    std::vector<double> figures;
    for (int i = 0; i < 180; ++i) {
        if (sampler.addFrame(1.0 / 60.0)) {
            figures.push_back(sampler.fps());
        }
    }
    for (int i = 0; i < 90; ++i) {
        if (sampler.addFrame(1.0 / 30.0)) {
            figures.push_back(sampler.fps());
        }
    }
    ASSERT_GT(figures.size(), 8u);

    int  mixed    = 0;
    int  first_30 = -1;
    int  last_60  = -1;
    for (std::size_t i = 0; i < figures.size(); ++i) {
        const double figure = figures[i];
        if (figure > 30.0 + 1e-9 && figure < 60.0 - 1e-9) {
            ++mixed;
            continue;
        }
        EXPECT_TRUE(std::abs(figure - 60.0) < 1e-9 || std::abs(figure - 30.0) < 1e-9)
            << "a figure is either a rate the stream really held, or the exact mix of the one window that "
               "straddled the change - never anything else (figure: "
            << figure << ")";
        if (figure < 31.0) {
            if (first_30 < 0) {
                first_30 = static_cast<int>(i);
            }
        } else {
            last_60 = static_cast<int>(i);
        }
    }
    EXPECT_EQ(mixed, 1) << "only the window that straddles the change may mix the two rates";
    EXPECT_DOUBLE_EQ(figures.front(), 60.0);
    EXPECT_DOUBLE_EQ(figures.back(), 30.0);
    EXPECT_GE(last_60, 3) << "60 fps really held for 3 s: several windows must say so";
    EXPECT_LE(first_30, last_60 + 2) << "the change is shown within about one window of happening";
}

TEST(FpsOverlayTest, AStallIsReportedAtItsTrueCostNotHidden)
{
    // A 50 ms frame inside an otherwise 60 fps window is a real cost - that half-second really averaged
    // 56.25 - and the readout's job is to say so rather than hide it behind the neighbouring 60s.
    FpsOverlay::Sampler sampler;
    for (int i = 0; i < 29; ++i) {
        sampler.addFrame(1.0 / 60.0);
    }
    ASSERT_TRUE(sampler.addFrame(0.050)) << "30 frames over 29/60 + 0.05 s is one full window";
    EXPECT_NEAR(sampler.fps(), 30.0 / (29.0 / 60.0 + 0.050), 1e-9);
    EXPECT_NEAR(sampler.fps(), 56.25, 1e-9);

    // And the next window is 60 again: the stall is not smeared forward either.
    for (int i = 0; i < 31; ++i) {
        sampler.addFrame(1.0 / 60.0);
    }
    EXPECT_NEAR(sampler.fps(), 60.0, 1e-9);
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
    // A HUD overlay draws over the main content: what says so is the DECLARATION that shows it (no clear, depth
    // disabled), which the builder writes - see RenderPipelineBuilderTest.TheHudOverlaysAreDeclarationsToo.
}

TEST(AxisGizmoTest, ViewportPlacedBottomLeft)
{
    auto gizmo = intrusive_ptr<AxisGizmo>(new AxisGizmo());
    gizmo->onSurfaceResized(800, 600);

    const Viewport vp = gizmo->boxViewport();
    EXPECT_EQ(vp.width, 96);
    EXPECT_EQ(vp.height, 96);
    EXPECT_EQ(vp.x, 16);
    EXPECT_EQ(vp.y, 600 - 16 - 96);
    // The box is square, so the camera that frames the sticks is projected for a square picture - stated where the
    // camera is made (see Camera and AxisGizmo's constructor) rather than derived from this rectangle later.
    EXPECT_DOUBLE_EQ(gizmo->camera()->aspectRatio(), 1.0);
}

TEST(AxisGizmoTest, ASurfaceTooSmallForTheBoxNeverYieldsANegativeViewport)
{
    // A window mid-layout can be smaller than the corner box plus its margins: measured 100x30 on the
    // demo's first shown frame, where the naive fit computed -2 pixels for the box side (see
    // .ai/design/vsg-reimplementation.md §11.16db). "No room" must be spelled as a zero-area rectangle
    // - the backend's empty-rectangle rule then skips the drawing call - never as a negative one.
    auto gizmo = intrusive_ptr<AxisGizmo>(new AxisGizmo());
    gizmo->onSurfaceResized(100, 30);

    const Viewport vp = gizmo->boxViewport();
    EXPECT_EQ(vp.width, 0);
    EXPECT_EQ(vp.height, 0);

    // A surface that DOES fit the box still gets it: the clamp only bites where the box cannot fit.
    gizmo->onSurfaceResized(100, 128);
    const Viewport fitted = gizmo->boxViewport();
    EXPECT_EQ(fitted.width, 96);
    EXPECT_EQ(fitted.height, 96);
    EXPECT_EQ(fitted.y, 128 - 16 - 96);
}

TEST(AxisGizmoTest, OrientationMirrorTracksSource)
{
    auto gizmo = intrusive_ptr<AxisGizmo>(new AxisGizmo());
    auto src = intrusive_ptr<Camera>(new Camera());
    src->setViewMatrixAsLookAt(Vec3d(2, 3, 6), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    gizmo->setSourceCamera(src.get());
    // The mirror is applied by update() (the hook the declaration that shows the gizmo calls); no backend is
    // needed to observe the framing camera update.
    gizmo->update();

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
    auto geometry = geometryOf(*makeUnitTriangle());
    geometry->setMaterial(intrusive_ptr<Material>(new Material()));
    auto group = intrusive_ptr<Group>(new Group());
    group->addChild(geometry);
    auto scene = intrusive_ptr<Scene>(new Scene());
    scene->setRoot(group);

    auto camera = intrusive_ptr<Camera>(new Camera());
    camera->setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    camera->setProjectionMatrixAsPerspective(45.0, 0.1, 100.0);

    auto backend = intrusive_ptr<MockBackend>(new MockBackend());

    // Without an override each command keeps its effective (per-geometry)
    // program; here it is null (no program set).
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(camera);
    pass->execute(scene.get(), backend.get());
    ASSERT_EQ(backend->last_programs.size(), 1u);
    EXPECT_EQ(backend->last_programs[0], nullptr);

    // A pass-level override forces every command onto one program, so the
    // same content scene can be re-rendered with a different shader.
    auto override_program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    auto overridden       = intrusive_ptr<RenderPass>(new RenderPass());
    overridden->setCamera(camera);
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

    explicit MockBackendFactory(const vn::String& name)
      : name_(name)
    {
    }

    RenderBackendInfo info() const override
    {
        const auto name = name_.empty() ? u8"mock3" : name_;
        return RenderBackendInfo{ name, name, u8"mock backend", u8"1.0.0", u8"test",
                                  RenderApi::Vulkan | RenderApi::OpenGL3 };
    }

    vn::intrusive_ptr<RenderBackend> create() override
    {
        return vn::intrusive_ptr<RenderBackend>(new MockBackend());
    }

  private:
    vn::String name_;
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
            EXPECT_TRUE(vn::testFlag(entry.info.api_flags, RenderApi::Vulkan));
            EXPECT_TRUE(vn::testFlag(entry.info.api_flags, RenderApi::OpenGL3));
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
    void updateMaterial(vn::raw_ptr<Material> material) override
    {
        if (material != nullptr) {
            registered_.insert(material);
        }
    }

    void releaseMaterial(vn::raw_ptr<Material> material) override
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

    bool hasMaterial(vn::raw_ptr<Material> material) const override
    {
        return registered_.count(material) != 0u;
    }

    void forEachMaterial(const std::function<void(vn::raw_ptr<Material>)>& visitor) const override
    {
        for (vn::raw_ptr<Material> m : registered_) {
            visitor(m);
        }
    }

  private:
    std::set<vn::raw_ptr<Material>> registered_;
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
    std::set<vn::raw_ptr<Material>> seen;
    manager.forEachMaterial([&seen](vn::raw_ptr<Material> m) { seen.insert(m); });
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

TEST(RenderStateTest, ThePriorityTableIsOnePlaceAndSaysWhoDecidedEachItem)
{
    // THE table: pass override > content (the folded StateNodes) > default, item by item, with the layer that
    // decided each one reported next to the value. One function, one place (StateNode.hpp's resolveRenderState),
    // so "why is this drawn in Line?" is a fact rather than an exercise in reading three files.
    RenderState content;
    content.cullMode    = CullMode::Back;
    content.blend       = BlendState{ .enabled = true };
    content.polygonMode = PolygonMode::Line;
    content.topology    = Topology::Lines;

    PassRenderState pass;
    pass.polygonMode = PolygonMode::Point;

    const ResolvedCommandState resolved = resolveRenderState(content, pass);

    // What the content authored is kept, and it says so.
    EXPECT_EQ(resolved.state.cullMode, CullMode::Back);
    EXPECT_EQ(resolved.origin.cullMode, StateOrigin::Content);
    EXPECT_TRUE(resolved.state.blend.enabled);
    EXPECT_EQ(resolved.origin.blend, StateOrigin::Content);
    EXPECT_EQ(resolved.state.topology, Topology::Lines);
    EXPECT_EQ(resolved.origin.topology, StateOrigin::Content);
    // ... except the one item the pass OVERRIDES: rasterisation is a viewing switch, so it wins even over a
    // content-authored Line (otherwise "back to shaded" could never be reached).
    EXPECT_EQ(resolved.state.polygonMode, PolygonMode::Point);
    EXPECT_EQ(resolved.origin.polygonMode, StateOrigin::Pass);

    // Depth is the asymmetric one: the pass states a POLICY for content that authored none...
    const ResolvedCommandState from_pass = resolveRenderState(RenderState{}, PassRenderState{ .depth = DepthMode::TestOnly });
    EXPECT_TRUE(from_pass.state.depth.test);
    EXPECT_FALSE(from_pass.state.depth.write);
    EXPECT_EQ(from_pass.origin.depth, StateOrigin::Pass);
    // ... and the CONTENT's word wins where it has one, compare op included (the pass has no opinion about it).
    DepthState authored_depth;
    authored_depth.compare = CompareOp::Always;
    RenderState depth_content;
    depth_content.depth = authored_depth;
    const ResolvedCommandState authored = resolveRenderState(depth_content, PassRenderState{ .depth = DepthMode::Disabled });
    EXPECT_TRUE(authored.state.depth.test) << "an authored depth is kept even where the pass says 'no depth'";
    EXPECT_EQ(authored.state.depth.compare, CompareOp::Always);
    EXPECT_EQ(authored.origin.depth, StateOrigin::Content);

    // Nobody said anything: the engine's defaults, and the origins say so (per item).
    const ResolvedCommandState defaults = resolveRenderState(RenderState{}, PassRenderState{});
    EXPECT_TRUE(defaults.state.depth.test);
    EXPECT_TRUE(defaults.state.depth.write);
    EXPECT_EQ(defaults.state.depth.compare, CompareOp::Less);
    EXPECT_EQ(defaults.state.cullMode, CullMode::None);
    EXPECT_FALSE(defaults.state.blend.enabled);
    EXPECT_EQ(defaults.state.polygonMode, PolygonMode::Fill);
    EXPECT_EQ(defaults.state.topology, Topology::Triangles);
    EXPECT_EQ(defaults.origin.cullMode, StateOrigin::Default);
    EXPECT_EQ(defaults.origin.blend, StateOrigin::Default);
    EXPECT_EQ(defaults.origin.polygonMode, StateOrigin::Default);
    EXPECT_EQ(defaults.origin.topology, StateOrigin::Default);

    // The content-only spelling answers exactly like the same call with a pass that overrides nothing.
    const ResolvedRenderState alone = resolveRenderState(content);
    const ResolvedCommandState with_empty_pass = resolveRenderState(content, PassRenderState{});
    EXPECT_EQ(alone, with_empty_pass.state);
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
    // The command carries what the CONTENT authored; the pass completes it (and the default pass state changes
    // nothing here, so the resolution below is the whole picture).
    const auto resolved = resolveRenderState(commands[0].authoredState, PassRenderState{});
    EXPECT_EQ(resolved.state.cullMode, CullMode::Back);
    EXPECT_EQ(resolved.origin.cullMode, StateOrigin::Content);
    EXPECT_TRUE(resolved.state.depth.test);
    EXPECT_FALSE(resolved.state.depth.write);
    EXPECT_EQ(resolved.origin.depth, StateOrigin::Content);
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
    ASSERT_TRUE(commands[0].authoredState.polygonMode.has_value());
    EXPECT_EQ(*commands[0].authoredState.polygonMode, PolygonMode::Point) << "the deeper StateNode authored it";
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
    // "Nobody said anything" is a fact of its own, and it is NOT the same as "said the default": the command
    // carries empty items, and the defaults appear in the resolution (where a pass could have said something).
    EXPECT_TRUE(commands[0].authoredState.empty());
    const auto resolved = resolveRenderState(commands[0].authoredState, PassRenderState{});
    EXPECT_TRUE(resolved.state.depth.test);
    EXPECT_TRUE(resolved.state.depth.write);
    EXPECT_EQ(resolved.state.cullMode, CullMode::None);
    EXPECT_EQ(resolved.state.polygonMode, PolygonMode::Fill);
    EXPECT_EQ(resolved.origin.cullMode, StateOrigin::Default) << "and it says so, per item";
    EXPECT_EQ(resolved.origin.topology, StateOrigin::Default);
}

TEST(GeometryTest, RawPositionsDriveCountsAndBounds)
{
    Geometry geom;
    EXPECT_FALSE(geom.hasPositions());
    EXPECT_FALSE(geom.hasIndices());
    EXPECT_EQ(geom.vertexCount(), 0u);
    EXPECT_TRUE(geom.boundingBox().isEmpty());

    vn::geometry::Vec3fArray points = {
        vn::math::Vec3f(0.0f, 0.0f, 0.0f),
        vn::math::Vec3f(2.0f, 0.0f, 0.0f),
        vn::math::Vec3f(0.0f, 3.0f, 0.0f),
    };
    geom.setPositions(packAttribute(points));
    EXPECT_TRUE(geom.hasPositions());
    EXPECT_EQ(geom.positionCount(), 3u);
    EXPECT_EQ(geom.vertexCount(), 3u);

    const Aabbd box = geom.boundingBox();
    EXPECT_TRUE(box.isValid());
    EXPECT_NEAR(box.min().x, 0.0, 1e-9);
    EXPECT_NEAR(box.max().x, 2.0, 1e-9);
    EXPECT_NEAR(box.max().y, 3.0, 1e-9);

    vn::geometry::UInt32Array indices = { 0u, 1u, 2u };
    geom.setIndices(packIndices(indices));
    EXPECT_TRUE(geom.hasIndices());
    EXPECT_FALSE(geom.indices().empty());
    EXPECT_EQ(geom.indices().size(), 3u);
    EXPECT_EQ(geom.vertexCount(), 3u);
}

TEST(GeometryTest, NormalsChannelAndRevision)
{
    // A data change is ANNOUNCED, never inferred: the setters change what the geometry holds and leave the
    // revision alone, so a renderer only rebuilds on what the caller reported (see setRevision()).
    Geometry geom;
    EXPECT_FALSE(geom.hasNormals());
    EXPECT_EQ(geom.revision(), 0u);

    vn::geometry::Vec3fArray points = { vn::math::Vec3f(0, 0, 0), vn::math::Vec3f(1, 0, 0),
                                          vn::math::Vec3f(0, 1, 0) };
    geom.setPositions(packAttribute(points));
    EXPECT_EQ(geom.revision(), 0u) << "setting a channel is not an announcement";

    vn::geometry::Vec3fArray normals = { vn::math::Vec3f(0, 0, 1), vn::math::Vec3f(0, 0, 1),
                                           vn::math::Vec3f(0, 0, 1) };
    geom.setNormals(packAttribute(normals));
    EXPECT_TRUE(geom.hasNormals());
    EXPECT_EQ(geom.normalCount(), 3u);
    EXPECT_EQ(geom.revision(), 0u);

    // The announcement is the only thing that moves it — for adding and removing alike.
    geom.setRevision(1u);
    EXPECT_EQ(geom.revision(), 1u);
    geom.removeBuffer(1);
    EXPECT_FALSE(geom.hasNormals());
    EXPECT_EQ(geom.revision(), 1u) << "removing a channel is not an announcement either";
}

TEST(GeometryTest, RevisionCanBeReportedByHand)
{
    // The geometry BORROWS the model's buffers, so a model rebuilt underneath it changes the bytes this
    // geometry reads without calling any setter here. Reporting that is the ONLY way a retained render
    // node can hear about it — and it is a value, not a bump, so it can mirror the model's version.
    Geometry geom;
    vn::geometry::Vec3fArray points = { vn::math::Vec3f(0, 0, 0) };
    geom.setPositions(packAttribute(points));

    geom.setRevision(41u);
    EXPECT_EQ(geom.revision(), 41u);

    // A setter afterwards does NOT move it: the announcement belongs to the caller, so a stale report
    // cannot quietly become fresh again (nor is a fresh one lost).
    geom.setIndices(packIndices(std::vector<std::uint32_t>{ 0u }));
    EXPECT_TRUE(geom.hasIndices());
    EXPECT_EQ(geom.revision(), 41u);
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

    // The converter is the Shape -> vertex-data bridge; a geometry filled by
    // the per-channel setters must carry exactly the same data.
    Geometry via_setters;
    via_setters.setPositions(packAttribute(mesh->positions()));
    EXPECT_TRUE(via_setters.hasPositions());
    EXPECT_EQ(via_setters.vertexCount(), 3u);
    EXPECT_EQ(via_setters.positionCount(), geom->positionCount());
    EXPECT_EQ(via_setters.hasIndices(), geom->hasIndices());
    EXPECT_EQ(via_setters.hasNormals(), geom->hasNormals());
}

TEST(GeometryTest, ConverterSharesTheMeshVertexStorage)
{
    auto mesh = intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->setPositions({ vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                         vn::math::Vec3f(0.0f, 1.0f, 0.0f) });
    mesh->setNormals({ vn::math::Vec3f(0.0f, 0.0f, 1.0f), vn::math::Vec3f(0.0f, 0.0f, 1.0f),
                       vn::math::Vec3f(0.0f, 0.0f, 1.0f) });
    mesh->setTexcoords({ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                         vn::math::Vec2f(0.0f, 1.0f) });

    const auto geom = geometryFromShape(*mesh);
    ASSERT_NE(geom.get(), nullptr);

    const AttributeChannel* positions = geom->buffer(0);
    const AttributeChannel* normals   = geom->buffer(1);
    const AttributeChannel* uvs       = geom->buffer(Geometry::kTexCoordLocation);
    ASSERT_NE(positions, nullptr);
    ASSERT_NE(normals, nullptr);
    ASSERT_NE(uvs, nullptr);

    // ONE allocation per attribute: the geometry reads the mesh's own scalars instead of a repacked copy.
    EXPECT_EQ(positions->scalars().data(), mesh->positionsBuffer()->data());
    EXPECT_EQ(normals->scalars().data(), mesh->normalsBuffer()->data());
    EXPECT_EQ(uvs->scalars().data(), mesh->texcoordsBuffer()->data());
    EXPECT_EQ(positions->components, 3u);
    EXPECT_EQ(uvs->components, 2u);

    // ...and reading through the geometry yields the mesh's own coordinates.
    const std::array<float, 3> second = positions->xyz(1);
    EXPECT_FLOAT_EQ(second[0], 1.0f);
    EXPECT_FLOAT_EQ(second[1], 0.0f);
    EXPECT_FLOAT_EQ(second[2], 0.0f);

    // The typed mesh view and the scalar run the geometry reads are the same bytes.
    EXPECT_EQ(mesh->positions().data(), reinterpret_cast<const vn::math::Vec3f*>(positions->scalars().data()));
    EXPECT_EQ(mesh->positions()[1].x, second[0]);

    EXPECT_EQ(geom->positionCount(), 3u);
    EXPECT_EQ(geom->normalCount(), 3u);
    EXPECT_EQ(geom->texcoordCount(), 3u);
}

TEST(GeometryTest, ConverterSharesTheMeshIndexStorage)
{
    auto mesh = intrusive_ptr<vn::geometry::IndexedTriangleMesh>(new vn::geometry::IndexedTriangleMesh());
    mesh->addVertex(vn::math::Vec3f(0.0f, 0.0f, 0.0f));
    mesh->addVertex(vn::math::Vec3f(1.0f, 0.0f, 0.0f));
    mesh->addVertex(vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->addTriangle(0u, 1u, 2u);

    const auto geom = geometryFromShape(*mesh);
    ASSERT_NE(geom.get(), nullptr);
    ASSERT_TRUE(geom->hasIndices());

    // The index stream is shared too: one allocation, read by both sides.
    EXPECT_EQ(geom->indices().data(), mesh->indicesBuffer()->data());
    EXPECT_EQ(geom->indices().size(), 3u);
    EXPECT_EQ(geom->indices()[2], 2u);

    // ...and because the geometry holds the buffer rather than a snapshot, the mesh appending another
    // triangle is followed instead of dangled.
    mesh->addTriangle(2u, 1u, 0u);
    EXPECT_EQ(geom->indices().size(), 6u);
    EXPECT_EQ(geom->indices().data(), mesh->indicesBuffer()->data());
}

TEST(GeometryTest, TexcoordChannelUsesTheCanonicalLocation)
{
    // The number is part of the contract only in the sense that it must stay
    // stable and must not collide with the custom-channel range the backend
    // forwards; the backend binds the array under vine_TexCoord0 by name.
    EXPECT_EQ(Geometry::kTexCoordLocation, 8u);

    Geometry geom;
    EXPECT_FALSE(geom.hasTexcoords());
    EXPECT_EQ(geom.texcoordCount(), 0u);
    const std::uint64_t before = geom.revision();

    geom.setTexcoords2(packAttribute(vn::geometry::Vec2fArray{ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                                                                vn::math::Vec2f(0.0f, 1.0f) }));

    EXPECT_TRUE(geom.hasTexcoords());
    EXPECT_EQ(geom.texcoordCount(), 3u);
    EXPECT_EQ(geom.revision(), before) << "a channel write is not an announcement (see setRevision())";

    const AttributeChannel* channel = geom.buffer(Geometry::kTexCoordLocation);
    ASSERT_NE(channel, nullptr);
    EXPECT_EQ(channel->components, 2u) << "a UV is two scalars per vertex, not three";
    EXPECT_EQ(channel->vertexCount(), 3u);
}

TEST(GeometryTest, ConverterCopiesTexcoordsWhenTheyMatchTheVertexCount)
{
    auto mesh = makeUnitTriangle();
    mesh->setTexcoords({ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                         vn::math::Vec2f(0.0f, 1.0f) });

    const auto geom = geometryFromShape(*mesh);
    ASSERT_NE(geom.get(), nullptr);
    EXPECT_TRUE(geom->hasTexcoords());
    EXPECT_EQ(geom->texcoordCount(), 3u);
}

TEST(GeometryTest, ConverterSkipsTexcoordsThatDoNotMatchTheVertexCount)
{
    auto mesh = makeUnitTriangle();
    // Two UVs for three vertices: a channel that cannot be indexed per vertex
    // is dropped rather than sampled against the wrong UV.
    mesh->setTexcoords({ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 1.0f) });

    const auto geom = geometryFromShape(*mesh);
    ASSERT_NE(geom.get(), nullptr);
    EXPECT_FALSE(geom->hasTexcoords());
}

TEST(GeometryTest, OpenAttributeChannelList)
{
    Geometry geom;
    EXPECT_EQ(geom.bufferCount(), 0u);
    EXPECT_TRUE(geom.bufferLocations().empty());
    const std::uint64_t base = geom.revision();

    // Any number of custom channels can be added at arbitrary locations.
    const AttributeChannel colour =
        AttributeChannel::packed({ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f }, 4u);
    geom.addBuffer(2, colour);
    const AttributeChannel size = AttributeChannel::packed({ 1.0f, 2.0f, 3.0f }, 1u);
    geom.addBuffer(4, size);

    EXPECT_EQ(geom.bufferCount(), 2u);
    ASSERT_NE(geom.buffer(2), nullptr);
    EXPECT_EQ(geom.buffer(2)->components, 4u);
    EXPECT_FALSE(geom.buffer(2)->empty());
    EXPECT_EQ(geom.buffer(2)->floatCount(), 8u);
    ASSERT_NE(geom.buffer(4), nullptr);
    EXPECT_EQ(geom.buffer(4)->components, 1u);
    EXPECT_EQ(geom.buffer(4)->floatCount(), 3u);
    EXPECT_EQ(geom.bufferLocations(), (std::vector<std::uint32_t>{ 2, 4 }));
    EXPECT_EQ(geom.revision(), base) << "adding channels is not an announcement";

    // Replacing a location keeps the count stable — and, like every setter, reports nothing.
    AttributeChannel replaced = colour;
    geom.addBuffer(2, replaced);
    EXPECT_EQ(geom.bufferCount(), 2u);
    EXPECT_EQ(geom.revision(), base);

    geom.removeBuffer(4);
    EXPECT_FALSE(geom.hasBuffer(4));
    EXPECT_EQ(geom.bufferLocations(), (std::vector<std::uint32_t>{ 2 }));
}


// ============ Attribute stride (components) ============

namespace
{

/**
 * @brief Builds a vec4 position channel (xyz + w) for a triangle.
 *
 * @param points Triangle vertices; each is padded to four components with w=1.
 * @return Position buffer with components = 4.
 */
AttributeChannel makeVec4Positions(const std::vector<vn::math::Vec3f>& points)
{
    std::vector<float> scalars;
    scalars.reserve(points.size() * 4u);
    for (const auto& p : points) {
        scalars.push_back(p.x);
        scalars.push_back(p.y);
        scalars.push_back(p.z);
        scalars.push_back(1.0f);
    }
    return AttributeChannel::packed(std::move(scalars), 4u);
}

}  // namespace

TEST(GeometryTest, AttributeChannelStrideAccessors)
{
    AttributeChannel buffer;
    EXPECT_EQ(buffer.stride(), 0u);
    EXPECT_EQ(buffer.vertexCount(), 0u);   // no data, no layout

    buffer.components = 4;
    EXPECT_EQ(buffer.vertexCount(), 0u);   // layout but still no data

    // Two complete vec4 vertices plus a trailing partial vertex: the partial
    // one is not a vertex.
    buffer = AttributeChannel::packed(
        { 1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 5.0f, 6.0f, 1.0f, 7.0f, 8.0f, 9.0f }, 4u);
    EXPECT_EQ(buffer.stride(), 4u);
    EXPECT_EQ(buffer.vertexCount(), 2u);

    const std::array<float, 3> first = buffer.xyz(0);
    EXPECT_FLOAT_EQ(first[0], 1.0f);
    EXPECT_FLOAT_EQ(first[1], 2.0f);
    EXPECT_FLOAT_EQ(first[2], 3.0f);
    const std::array<float, 3> second = buffer.xyz(1);
    EXPECT_FLOAT_EQ(second[0], 4.0f);
    EXPECT_FLOAT_EQ(second[1], 5.0f);
    EXPECT_FLOAT_EQ(second[2], 6.0f);

    // A zero stride carries no usable layout.
    buffer.components = 0;
    EXPECT_EQ(buffer.vertexCount(), 0u);
}

TEST(AttributeChannelTest, SharedChannelReadsTheBuffersOwnScalars)
{
    // The sharing design rests on this: a Vec3f is three tightly packed floats and a Vec2f is two, so a run
    // of scalars IS a run of vertices (this is also static_asserted in Mesh.cpp).
    static_assert(sizeof(vn::math::Vec3f) == 3u * sizeof(float));
    static_assert(sizeof(vn::math::Vec2f) == 2u * sizeof(float));

    auto buffer = intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f }));

    const AttributeChannel channel = AttributeChannel::shared(buffer, 3u);
    EXPECT_EQ(channel.components, 3u);
    EXPECT_EQ(channel.floatCount(), 6u);
    EXPECT_EQ(channel.vertexCount(), 2u);
    // The channel adds no storage of its own: it reads the buffer's scalars in place.
    EXPECT_EQ(channel.scalars().data(), buffer->data());

    const std::array<float, 3> second = channel.xyz(1);
    EXPECT_FLOAT_EQ(second[0], 4.0f);
    EXPECT_FLOAT_EQ(second[1], 5.0f);
    EXPECT_FLOAT_EQ(second[2], 6.0f);

    // The buffer is re-read, not snapshotted, so GROWING it is followed: the new length shows up and the view
    // is re-derived from the handle. A cached pointer + count would be stale here (and could point at freed
    // memory), which is exactly what holding the buffer instead of a snapshot avoids.
    buffer->append(std::vector<float>{ 7.0f, 8.0f, 9.0f });
    EXPECT_EQ(channel.floatCount(), 9u);
    EXPECT_EQ(channel.vertexCount(), 3u);
    EXPECT_EQ(channel.scalars().data(), buffer->data());
    EXPECT_FLOAT_EQ(channel.scalars()[8], 9.0f);

    // The channel has to keep the buffer alive — the scalars live inside it. (Reading them after dropping
    // the local handle would be a use-after-free, not a wrong value, if this reference were missing.)
    const vn::Buffer<float>* const raw = buffer.get();
    ASSERT_EQ(raw->useCount(), 2u);
    buffer = nullptr;
    EXPECT_EQ(raw->useCount(), 1u);
    EXPECT_FLOAT_EQ(channel.scalars()[3], 4.0f);
}

TEST(GeometryTest, Vec4PositionsUseComponentsAsStride)
{
    Geometry geom;
    // A triangle offset away from the origin, stored as vec4. A stride-blind
    // reader would treat the w = 1 values as vertex data: it would read 4
    // "vertices" out of 12 floats and fold w = 1 into the bounds.
    geom.addBuffer(0, makeVec4Positions({ vn::math::Vec3f(1.0f, 2.0f, 0.5f),
                                          vn::math::Vec3f(3.0f, 2.0f, 0.5f),
                                          vn::math::Vec3f(1.0f, 5.0f, 0.5f) }));

    EXPECT_EQ(geom.positionCount(), 3u);
    EXPECT_EQ(geom.vertexCount(), 3u);

    const Aabbd box = geom.boundingBox();
    ASSERT_TRUE(box.isValid());
    EXPECT_NEAR(box.min().x, 1.0, 1e-9);
    EXPECT_NEAR(box.max().x, 3.0, 1e-9);
    EXPECT_NEAR(box.min().y, 2.0, 1e-9);
    EXPECT_NEAR(box.max().y, 5.0, 1e-9);
    EXPECT_NEAR(box.min().z, 0.5, 1e-9);
    EXPECT_NEAR(box.max().z, 0.5, 1e-9);
}

TEST(GeometryTest, Vec4NormalsCountedByStride)
{
    Geometry geom;
    geom.addBuffer(0, makeVec4Positions({ vn::math::Vec3f(0.0f, 0.0f, 0.0f),
                                          vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                                          vn::math::Vec3f(0.0f, 1.0f, 0.0f) }));
    geom.addBuffer(1, makeVec4Positions({ vn::math::Vec3f(0.0f, 0.0f, 1.0f),
                                          vn::math::Vec3f(0.0f, 0.0f, 1.0f) }));
    EXPECT_EQ(geom.positionCount(), 3u);
    EXPECT_EQ(geom.normalCount(), 2u);
}

TEST(GeometryTest, ZeroStrideBufferBoundsNothing)
{
    Geometry geom;
    // A zero stride carries no usable layout for any vertex count.
    const AttributeChannel broken = AttributeChannel::packed({ 1.0f, 2.0f, 3.0f }, 0u);
    geom.addBuffer(0, broken);

    ASSERT_NE(geom.buffer(0), nullptr);
    EXPECT_EQ(geom.buffer(0)->vertexCount(), 0u);
    EXPECT_EQ(geom.positionCount(), 0u);
    // No usable positions -> empty bound, so nothing is culled out by accident
    // and nothing is drawn from a phantom vertex.
    EXPECT_TRUE(geom.boundingBox().isEmpty());
}

TEST(GeometryTest, SubThreeComponentChannelBoundsNothing)
{
    Geometry geom;
    // Two floats per vertex cannot carry xyz.
    const AttributeChannel flat = AttributeChannel::packed({ 1.0f, 2.0f, 3.0f, 4.0f }, 2u);
    geom.addBuffer(0, flat);

    EXPECT_EQ(geom.positionCount(), 2u);   // two 2-component vertices
    EXPECT_TRUE(geom.boundingBox().isEmpty());
}

TEST(RayIntersectionTest, PicksVec4PositionGeometry)
{
    // Regression: picking assumed three floats per vertex, so vec4 positions
    // were read as a corrupted triangle soup and the ray missed (or hit the
    // wrong place).
    auto scene = intrusive_ptr<Scene>(new Scene());
    auto root = intrusive_ptr<Group>(new Group());
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    geom->setName(u8"vec4-tri");
    geom->addBuffer(0, makeVec4Positions({ vn::math::Vec3f(0.0f, 0.0f, 0.0f),
                                           vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                                           vn::math::Vec3f(0.0f, 1.0f, 0.0f) }));
    root->addChild(geom);
    scene->setRoot(root);

    Ray ray(Vec3d(0.25, 0.25, 1.0), Vec3d(0, 0, -1));
    RayIntersectionResult hit = RayIntersection::intersectScene(ray, scene.get());
    EXPECT_TRUE(hit.hit);
    EXPECT_EQ(hit.geometry.get(), geom.get());
    EXPECT_NEAR(hit.distance, 1.0, 1e-6);
    EXPECT_NEAR(hit.point.x, 0.25, 1e-6);
    EXPECT_NEAR(hit.point.y, 0.25, 1e-6);

    // A ray outside the (correctly read) triangle still misses.
    Ray off_ray(Vec3d(5.0, 5.0, 1.0), Vec3d(0, 0, -1));
    EXPECT_FALSE(RayIntersection::intersectScene(off_ray, scene.get()).hit);
}

TEST(SceneTest, CollectCommandsKeepsVec4PositionGeometryInView)
{
    // The culling consequence of the stride fix: a vec4-position triangle
    // inside the frustum must be collected, which requires its bound to be
    // derived with the correct stride.
    Scene scene;
    auto root = setIdentityRoot(scene);
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    node->setMatrix(vn::math::translate(Vec3d(0.0, 0.0, -3.0)));
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    geom->setName(u8"vec4-tri");
    geom->addBuffer(0, makeVec4Positions({ vn::math::Vec3f(-0.5f, -0.5f, 0.0f),
                                           vn::math::Vec3f(0.5f, -0.5f, 0.0f),
                                           vn::math::Vec3f(0.0f, 0.5f, 0.0f) }));
    node->addChild(geom);
    root->addChild(node);

    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry->name(), u8"vec4-tri");

    // The same geometry pushed far outside the view is still culled, so the
    // fix did not disable culling for this data layout.
    node->setMatrix(vn::math::translate(Vec3d(400.0, 0.0, -3.0)));
    EXPECT_TRUE(scene.collectRenderCommands(&cam).empty());
}

// ============ Traversal: world accumulation, bounds, child access ============

TEST(NodeTest, ChildrenRefMirrorsChildrenInOrder)
{
    auto group = intrusive_ptr<Group>(new Group());
    auto first = intrusive_ptr<Node>(new Node());
    auto second = intrusive_ptr<Node>(new Node());
    first->setName(u8"first");
    second->setName(u8"second");
    group->addChild(second);
    group->addChild(first);

    const std::vector<NodePtr>& borrowed = group->childrenRef();
    ASSERT_EQ(borrowed.size(), 2u);
    EXPECT_EQ(borrowed[0].get(), second.get());   // insertion order
    EXPECT_EQ(borrowed[1].get(), first.get());

    // The copy-returning accessor stays in sync (same order, same nodes).
    const std::vector<NodePtr> copied = group->children();
    ASSERT_EQ(copied.size(), borrowed.size());
    for (std::size_t i = 0; i < borrowed.size(); ++i) {
        EXPECT_EQ(copied[i].get(), borrowed[i].get());
    }

    group->removeChild(second.get());
    EXPECT_EQ(group->childrenRef().size(), 1u);
    EXPECT_EQ(group->childrenRef()[0].get(), first.get());

    auto empty = intrusive_ptr<Group>(new Group());
    EXPECT_TRUE(empty->childrenRef().empty());
}

TEST(NodeTest, DeepHierarchyWorldMatrixFoldsWholeChain)
{
    // A 12-level chain of transforms: worldMatrix() must fold every level, in
    // root-to-leaf order, and match the reference product accumulated
    // top-down. The previous recursive form produced the same matrix but
    // recomputed the ancestor chain at each level (O(depth^2)); this pins the
    // semantics of the iterative form.
    auto root = intrusive_ptr<Group>(new Group());
    intrusive_ptr<Group> parent = root;
    Mat4d reference;   // identity
    std::vector<Mat4d> expected_world;
    std::vector<intrusive_ptr<MatrixTransform>> levels;

    for (int level = 0; level < 12; ++level) {
        auto transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
        const Mat4d local = vn::math::translate(Vec3d(0.1, 0.1, -0.2)) *
                            vn::math::rotate(Vec3d(0.0, 1.0, 0.0), 0.3);
        transform->setMatrix(local);
        parent->addChild(transform);
        reference = reference * local;
        expected_world.push_back(reference);
        parent = transform;
        levels.push_back(transform);
    }

    auto leaf = geometryOf(*makeUnitTriangle());
    parent->addChild(leaf);

    const auto close = [](const Mat4d& lhs, const Mat4d& rhs) {
        for (std::size_t row = 0; row < 4u; ++row) {
            for (std::size_t col = 0; col < 4u; ++col) {
                if (std::abs(lhs.element(row, col) - rhs.element(row, col)) > 1e-9) {
                    return false;
                }
            }
        }
        return true;
    };

    for (std::size_t i = 0; i < levels.size(); ++i) {
        EXPECT_TRUE(close(levels[i]->worldMatrix(), expected_world[i])) << "level " << i;
    }
    EXPECT_TRUE(close(leaf->worldMatrix(), expected_world.back()));

    // The collection pass bakes the same accumulated matrix into the command,
    // so the traversal's top-down accumulation matches worldMatrix().
    auto scene = intrusive_ptr<Scene>(new Scene());
    scene->setRoot(root);
    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene->collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_TRUE(close(commands[0].modelMatrix, leaf->worldMatrix()));
}

TEST(SceneTest, CollectCommandsAccumulatesWorldMatrixAcrossNesting)
{
    // Layout: root -> T1(+100 x, out of view) -> Group -> T2(-100 x, -3 z)
    // -> geometry. Each node's world matrix must be the product down the
    // chain: the group's bound is the geometry's world-space bound (not the
    // group's own translation), so the subtree is NOT culled, and the baked
    // matrix returns to the origin.
    Scene scene;
    auto root = setIdentityRoot(scene);
    auto outer = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    outer->setMatrix(vn::math::translate(Vec3d(100.0, 0.0, 0.0)));
    auto middle = intrusive_ptr<Group>(new Group());
    auto inner = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    inner->setMatrix(vn::math::translate(Vec3d(-100.0, 0.0, -3.0)));
    auto geom = geometryOf(*makeUnitTriangle());
    geom->setName(u8"nested");
    root->addChild(outer);
    outer->addChild(middle);
    middle->addChild(inner);
    inner->addChild(geom);

    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry->name(), u8"nested");
    EXPECT_NEAR(commands[0].modelMatrix.element(0, 3), 0.0, 1e-9);
    EXPECT_NEAR(commands[0].modelMatrix.element(2, 3), -3.0, 1e-9);

    // Moving the geometry genuinely out of view still culls it, i.e. the
    // container bound is being derived from the children rather than ignored.
    inner->setMatrix(vn::math::translate(Vec3d(-100.0, 0.0, 500.0)));
    EXPECT_TRUE(scene.collectRenderCommands(&cam).empty());
}

TEST(SceneTest, CollectCommandsCullsContainersByUnionOfChildren)
{
    // Two children of one group: one in view, one far away. The group's bound
    // covers both, so the group is not culled, but only the visible child
    // survives its own leaf test.
    Scene scene;
    auto root = setIdentityRoot(scene);
    auto group = intrusive_ptr<Group>(new Group());
    auto near_node = makeTriangleNode(Vec3d(0.0, 0.0, -3.0), nullptr, u8"near");
    auto far_node = makeTriangleNode(Vec3d(500.0, 0.0, -3.0), nullptr, u8"far");
    group->addChild(near_node);
    group->addChild(far_node);
    root->addChild(group);

    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry->name(), u8"near");
}

TEST(SceneTest, CollectCommandsPreservesCollectionOrderForEqualDepth)
{
    // The distance-sort key must keep std::stable_sort's tie-breaking: equal
    // distances stay in collection order (insertion order here), which user
    // code relies on for deterministic frames.
    Scene scene;
    auto root = setIdentityRoot(scene);
    auto first = makeTriangleNode(Vec3d(0.0, 0.0, -3.0), nullptr, u8"first");
    auto second = makeTriangleNode(Vec3d(0.0, 0.0, -3.0), nullptr, u8"second");
    auto third = makeTriangleNode(Vec3d(0.0, 0.0, -3.0), nullptr, u8"third");
    root->addChild(first);
    root->addChild(second);
    root->addChild(third);

    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 3u);
    EXPECT_EQ(commands[0].geometry->name(), u8"first");
    EXPECT_EQ(commands[1].geometry->name(), u8"second");
    EXPECT_EQ(commands[2].geometry->name(), u8"third");

    // Transparent siblings keep collection order too when equidistant.
    first->setOpacity(0.5f);
    second->setOpacity(0.5f);
    const auto transparent = scene.collectRenderCommands(&cam);
    ASSERT_EQ(transparent.size(), 3u);
    EXPECT_EQ(transparent[0].geometry->name(), u8"third");   // opaque batch
    EXPECT_FALSE(transparent[0].isTransparent());
    EXPECT_EQ(transparent[1].geometry->name(), u8"first");
    EXPECT_EQ(transparent[2].geometry->name(), u8"second");
}

namespace
{

/**
 * @brief Geometry that counts how often its bound is requested.
 */
class CountingGeometry : public Geometry
{
  public:
    mutable int bound_calls = 0;

    /**
     * @brief Counts the request, then defers to the real bound.
     *
     * @return World-space AABB of this geometry.
     */
    Aabbd boundingBox() const override
    {
        ++bound_calls;
        return Geometry::boundingBox();
    }
};

/**
 * @brief Group that counts how often its bound is requested.
 */
class CountingGroup : public Group
{
  public:
    mutable int bound_calls = 0;

    /**
     * @brief Counts the request, then defers to the real bound.
     *
     * @return World-space AABB of this subtree.
     */
    Aabbd boundingBox() const override
    {
        ++bound_calls;
        return Group::boundingBox();
    }
};

}  // namespace

TEST(SceneTest, CollectCommandsAsksEachLeafBoundOnce)
{
    // The pass culls with each node's world bound. Asking a container for its
    // bound re-walks its whole subtree (Group::boundingBox unions the
    // children), so doing that per visited node made collection quadratic in
    // the node count. The pass must therefore ask every leaf once — no matter
    // how deep it sits — and never ask a container.
    Scene scene;
    auto root = setIdentityRoot(scene);
    auto deep = root;
    intrusive_ptr<CountingGroup> counted_container;
    intrusive_ptr<MatrixTransform> first_transform;
    for (int level = 0; level < 8; ++level) {
        auto transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
        transform->setMatrix(vn::math::translate(Vec3d(0.0, 0.0, -0.1)));
        deep->addChild(transform);
        deep = transform;
        if (level == 0) {
            first_transform = transform;
        }
        if (level == 4) {
            counted_container = intrusive_ptr<CountingGroup>(new CountingGroup());
            deep->addChild(counted_container);
            deep = counted_container;
        }
    }
    auto leaf = intrusive_ptr<CountingGeometry>(new CountingGeometry());
    // CountingGeometry is a Geometry subclass, so fill it through the
    // per-channel setter (the Shape converter always returns a plain Geometry).
    leaf->setPositions(packAttribute(makeUnitTriangle()->positions()));
    deep->addChild(leaf);

    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(leaf->bound_calls, 1);
    EXPECT_EQ(counted_container->bound_calls, 0);

    // A second pass asks again (the cache lives for one collection only, so a
    // moved node is never culled with a stale bound).
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(leaf->bound_calls, 2);
    EXPECT_EQ(counted_container->bound_calls, 0);

    // Culled leaves are still bounded exactly once (the bound is what decides).
    leaf->setName(u8"culled");
    first_transform->setMatrix(vn::math::translate(Vec3d(4000.0, 0.0, 0.0)));
    EXPECT_TRUE(scene.collectRenderCommands(&cam).empty());
    EXPECT_EQ(leaf->bound_calls, 3);
}

namespace
{

/// @brief A camera that sees world x in [3.5, 7.5] and y in [-1.5, 2.5] - the region the invalidation
/// firewalls below move content into (a unit triangle at the origin is out of view for it). The ortho
/// bounds are CAMERA space, and the eye sits at x = 5.5, so the visible world window is x ~ [3.5, 7.5].
void pointCameraAtX5(Camera& camera)
{
    camera.setViewMatrixAsLookAt(Vec3d(5.5, 0.5, 5.0), Vec3d(5.5, 0.5, 0.0), Vec3d(0.0, 1.0, 0.0));
    camera.setProjectionMatrixAsOrtho(-2.0, 2.0, -2.0, 2.0, 0.5, 100.0);
}

/// @brief Sums Node::boundsRecomputeCount() over @p node's subtree (the per-node bounds cache's witness).
std::uint64_t sumBoundsRecomputes(const Node* node)
{
    std::uint64_t total = node->boundsRecomputeCount();
    if (const auto* group = dynamic_cast<const Group*>(node)) {
        for (const auto& child : group->childrenRef()) {
            total += sumBoundsRecomputes(child.get());
        }
    }
    return total;
}

/// @brief A node whose placement lives OUTSIDE the engine's setters - the case Node::invalidateBounds()
/// exists for: the node announces its own changes through the hook.
class ShiftedGroup : public Group
{
  public:
    void setShift(double x) noexcept
    {
        shift_ = x;
    }

    Mat4d localTransformMatrix() const override
    {
        return vn::math::translate(Vec3d(shift_, 0.0, 0.0));
    }

  private:
    double shift_ = 0.0;
};

}  // namespace

TEST(SceneTest, CachedSubtreeBoxesSurviveCameraMovesAndFollowAnnouncements)
{
    // P18 §10.2: each subtree's box in its OWN frame is cached per node, so it survives CAMERA moves -
    // a box in a node's own frame does not depend on the camera - and only an announcement inside the
    // subtree invalidates it. The witness is Node::boundsRecomputeCount(): a second collect through a
    // different camera recomputes nothing, while setMatrix() recomputes exactly the announced spine.
    Scene scene;
    auto  root      = setIdentityRoot(scene);
    auto  container = intrusive_ptr<Group>(new Group());
    auto  transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    transform->addChild(geometryOf(*makeUnitTriangle()));
    container->addChild(transform);
    root->addChild(container);

    Camera first;
    setupLookAtCamera(first); // looks at the origin: the unit triangle is in view
    Camera second;
    setupLookAtCamera(second);
    second.setProjectionMatrixAsPerspective(70.0, 0.1, 1000.0); // the camera MOVED: same scene, new view

    scene.setContentFrame(1);
    ASSERT_EQ(scene.collectRenderCommands(&first).size(), 1u);
    const std::uint64_t first_total = sumBoundsRecomputes(root.get());
    EXPECT_GT(first_total, 0u) << "the first collect computes the unions";

    scene.setContentFrame(2);
    EXPECT_EQ(scene.collectRenderCommands(&second).size(), 1u) << "the triangle is still in view";
    EXPECT_EQ(sumBoundsRecomputes(root.get()), first_total)
        << "a camera move must not recompute a single cached box";

    transform->setMatrix(vn::math::translate(Vec3d(0.0, 0.4, 0.0)));
    scene.setContentFrame(3);
    (void)scene.collectRenderCommands(&first);
    EXPECT_GT(sumBoundsRecomputes(root.get()), first_total) << "the announced spine recomputes";
}

TEST(SceneTest, ASubtreeMovedIntoViewIsNotHiddenByAStaleBox)
{
    // The failure a missed announcement causes is a DISAPPEARING OBJECT, not a slow frame: a stale
    // container box culls content that moved into the camera. The triangle sits at x in [0, 1]
    // (correctly out of view); moving it to x = 5 must bring it back.
    Scene scene;
    auto  root      = setIdentityRoot(scene);
    auto  container = intrusive_ptr<Group>(new Group());
    auto  transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    transform->addChild(geometryOf(*makeUnitTriangle()));
    container->addChild(transform);
    root->addChild(container);

    Camera camera;
    pointCameraAtX5(camera);

    scene.setContentFrame(1);
    EXPECT_TRUE(scene.collectRenderCommands(&camera).empty()) << "x in [0, 1] is out of view";

    transform->setMatrix(vn::math::translate(Vec3d(5.0, 0.0, 0.0)));
    scene.setContentFrame(2);
    EXPECT_EQ(scene.collectRenderCommands(&camera).size(), 1u)
        << "the moved subtree must be found again: a stale box would hide it";
}

TEST(SceneTest, AChildAddedLaterExtendsItsParentsBox)
{
    Scene scene;
    auto  root      = setIdentityRoot(scene);
    auto  container = intrusive_ptr<Group>(new Group());
    root->addChild(container);

    Camera camera;
    pointCameraAtX5(camera);

    scene.setContentFrame(1);
    EXPECT_TRUE(scene.collectRenderCommands(&camera).empty()) << "an empty container draws nothing";

    auto transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    transform->setMatrix(vn::math::translate(Vec3d(5.0, 0.0, 0.0)));
    transform->addChild(geometryOf(*makeUnitTriangle()));
    container->addChild(transform);
    scene.setContentFrame(2);
    EXPECT_EQ(scene.collectRenderCommands(&camera).size(), 1u)
        << "addChild announces: a stale empty box would hide the new child";
}

TEST(SceneTest, AnEditedGeometryAnnouncesItsBoundsThroughItsSetters)
{
    Scene scene;
    auto  root      = setIdentityRoot(scene);
    auto  container = intrusive_ptr<Group>(new Group());
    auto  transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto  leaf      = geometryOf(*makeUnitTriangle());
    transform->addChild(leaf);
    container->addChild(transform);
    root->addChild(container);

    Camera camera;
    pointCameraAtX5(camera);

    scene.setContentFrame(1);
    EXPECT_TRUE(scene.collectRenderCommands(&camera).empty()) << "x in [0, 1] is out of view";

    // The host replaces the data: the triangle moves +5 on X in the geometry's own frame, and the
    // revision announcement is what tells every cached box above the leaf to let go.
    leaf->setPositions(intrusive_ptr<const vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 5.0F, 0.0F, 0.0F, 6.0F, 0.0F, 0.0F, 5.0F, 1.0F, 0.0F })));
    leaf->bumpRevision();
    scene.setContentFrame(2);
    EXPECT_EQ(scene.collectRenderCommands(&camera).size(), 1u)
        << "the data edit must reach the boxes above: an unannounced edit would hide the object";
}

TEST(SceneTest, ACustomPlacementAnnouncesThroughInvalidateBounds)
{
    // A node whose local matrix comes from its own state is outside every built-in setter: the
    // documented hook is Node::invalidateBounds(), called after the change - the same announce pattern
    // the rest of the SDK follows (Geometry::bumpRevision for data). The hook is what makes the custom
    // case part of the contract instead of a silent stale box.
    Scene scene;
    auto  root   = setIdentityRoot(scene);
    auto  custom = intrusive_ptr<ShiftedGroup>(new ShiftedGroup());
    custom->addChild(geometryOf(*makeUnitTriangle()));
    root->addChild(custom);

    Camera camera;
    pointCameraAtX5(camera);

    scene.setContentFrame(1);
    EXPECT_TRUE(scene.collectRenderCommands(&camera).empty()) << "x in [0, 1] is out of view";

    custom->setShift(5.0);
    custom->invalidateBounds();
    scene.setContentFrame(2);
    EXPECT_EQ(scene.collectRenderCommands(&camera).size(), 1u)
        << "the announced custom placement must be picked up";
}

TEST(SceneTest, TheSharedCollectionIsOneListAndTheOwningSpellingCopiesIt)
{
    // The passes of one frame draw the SAME commands, so the engine's pass path takes the list by
    // reference (collectRenderCommandsShared): copying it per pass cost an intrusive_ptr increment per
    // command (three of them) plus a command-sized memcpy, on every pass of every frame, for a list
    // nobody was going to change. What must NOT change is the other half of that contract: a caller
    // that DOES change the list gets its own copy (that is what the pass-level program override relies
    // on, and it must not reach the other passes sharing the frame's walk).
    Scene scene;
    auto  root    = setIdentityRoot(scene);
    auto  triangle = geometryOf(*makeUnitTriangle());
    root->addChild(triangle);

    Camera cam;
    setupLookAtCamera(cam);

    scene.setContentFrame(1);
    const auto first  = scene.collectRenderCommandsShared(&cam);
    const auto second = scene.collectRenderCommandsShared(&cam);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first.get(), second.get()) << "one walk builds one list, and every pass shares it";
    EXPECT_EQ(first->size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 1u) << "the second call reused the frame's list";
    EXPECT_EQ(scene.contentCollectReuseCount(), 1u);

    // The owning spelling hands out a COPY: editing it is a pass' own business.
    auto owned = scene.collectRenderCommands(&cam);
    ASSERT_EQ(owned.size(), first->size());
    owned[0].opacity = 0.25f;
    owned.clear();
    EXPECT_EQ(first->size(), 1u) << "the shared list is untouched by a caller's own list";
    EXPECT_NE((*first)[0].opacity, 0.25f) << "a pass' edit must not reach the frame's shared list";
    EXPECT_EQ(scene.collectRenderCommandsShared(&cam).get(), first.get());

    // Outside a content frame nothing is memoised, so each call builds its own list: a caller that
    // drives the collection itself keeps the plain walk-on-every-call behaviour.
    Scene plain;
    auto  plain_root = setIdentityRoot(plain);
    plain_root->addChild(geometryOf(*makeUnitTriangle()));
    const auto plain_first  = plain.collectRenderCommandsShared(&cam);
    const auto plain_second = plain.collectRenderCommandsShared(&cam);
    EXPECT_NE(plain_first.get(), plain_second.get());
    EXPECT_EQ(plain.contentCollectCount(), 2u);

    // And a scene with nothing to draw hands out the shared EMPTY list, not a fresh allocation per
    // call: asking "nothing" is the one question a caller asks most often.
    Scene empty;
    const auto empty_first  = empty.collectRenderCommandsShared(&cam);
    const auto empty_second = empty.collectRenderCommandsShared(nullptr);
    EXPECT_TRUE(empty_first->empty());
    EXPECT_EQ(empty_first.get(), empty_second.get());
}

TEST(SceneTest, TheLocalDataBoxIsComputedOnceAndRecomputedWhenTheDataChanges)
{
    // A collection pass asks EVERY geometry for its bound on every frame it has to walk the tree — a
    // moving camera fills the frame memo for nothing — and the bound costs a pass over every vertex of
    // the mesh. So the local data box is remembered, keyed by the stream it was computed from and by the
    // geometry's own revision: a writer that announces an edit (the codebase's standing contract) gets a
    // fresh box, and everything else pays one scan instead of one per frame.
    Scene scene;
    auto  root     = setIdentityRoot(scene);
    auto  geometry = vn::make_intrusive<Geometry>();
    // A writable handle as well as the channel's: announcing an edit is the WRITER's job, and a channel
    // hands its buffer out as const (the geometry reads it, it does not own the edit).
    auto positions = packAttribute(makeUnitTriangle()->positions());
    geometry->setPositions(positions);
    root->addChild(geometry);

    Camera cam;
    setupLookAtCamera(cam);

    EXPECT_EQ(geometry->localBoundsComputationCount(), 0u) << "nothing asked yet, nothing computed";
    ASSERT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(geometry->localBoundsComputationCount(), 1u);

    // A second walk asks for the bound again (the cache lives for one collection) but does not scan the
    // vertices again — this is the per-frame cost the cache removes, not the per-call one.
    ASSERT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(geometry->localBoundsComputationCount(), 1u);

    // Moving the node recomputes nothing: the LOCAL box is what was cached, the world box is derived.
    const Aabbd local = geometry->boundingBox();
    const Aabbd moved = [&] {
        auto transform = intrusive_ptr<MatrixTransform>(new MatrixTransform());
        transform->setMatrix(vn::math::translate(Vec3d(0.1, 0.0, 0.0)));
        transform->addChild(geometry);
        root->addChild(transform);
        return geometry->boundingBox();
    }();
    EXPECT_EQ(geometry->localBoundsComputationCount(), 1u) << "a translation is not a data change";
    EXPECT_NEAR(local.center().x, moved.center().x - 0.1, 1e-9) << "and the world box did move";

    // An edit announced at the GEOMETRY level (the "I changed the data" call every writer makes).
    geometry->setRevision(geometry->revision() + 1);
    ASSERT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(geometry->localBoundsComputationCount(), 2u) << "an announced edit re-derives the box";

    // ...and one announced on the STREAM, which is how a buffer edited in place is announced.
    positions->setRevision(positions->revision() + 1);
    ASSERT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(geometry->localBoundsComputationCount(), 3u);

    // A different SEGMENT of the same buffer is a different stream, so it is a different box: an arena
    // holds several geometries' vertices, and sharing one cached box between them would cull them together.
    geometry->setPositions(positions, 0u, 2u);
    ASSERT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(geometry->localBoundsComputationCount(), 4u);
}

TEST(SceneTest, TheFoldedStateAgreesWithTheUpWalkingHelpers)
{
    // The collection folds the shading state a subtree inherits on the way DOWN (one pass, O(1) per node,
    // no ancestor walk and no per-leaf allocation), while StateNode.hpp's helpers compute the same thing by
    // walking UP from the leaf. Two spellings of one rule is one spelling too many unless they are pinned
    // to agree — and this tree makes every override actually override something: two StateNodes with
    // different cull modes and depth, a program on the inner one, a material on the outer one, and a leaf
    // that authors its own material.
    Scene scene;
    auto  root   = setIdentityRoot(scene);
    auto  outer  = vn::make_intrusive<StateNode>();
    auto  inner  = vn::make_intrusive<StateNode>();
    auto  geometry = vn::make_intrusive<Geometry>();
    geometry->setPositions(packAttribute(makeUnitTriangle()->positions()));

    auto outer_material = vn::make_intrusive<Material>();
    auto own_material   = vn::make_intrusive<Material>();
    auto inner_program  = flatForwardProgram();
    ASSERT_NE(inner_program, nullptr);

    outer->setCullMode(CullMode::Back);
    outer->setMaterial(outer_material);
    inner->setCullMode(CullMode::Front);
    inner->setDepth(DepthState{ false, false, CompareOp::Always });
    inner->setProgram(inner_program);
    geometry->setMaterial(own_material);

    root->addChild(outer);
    outer->addChild(inner);
    inner->addChild(geometry);

    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    const RenderCommand& command = commands[0];

    // The folded state, item by item, against the SDK's own up-walking spelling.
    const RenderState         folded   = collectRenderState(geometry.get());
    const ResolvedRenderState expected = effectiveRenderState(geometry.get());
    EXPECT_EQ(command.authoredState, folded) << "the command carries exactly what the fold said";
    const ResolvedCommandState resolved = resolveRenderState(command.authoredState, PassRenderState{});
    EXPECT_EQ(resolved.state.cullMode, expected.cullMode) << "the DEEPER StateNode's cull mode wins";
    EXPECT_EQ(resolved.state.cullMode, CullMode::Front);
    EXPECT_EQ(resolved.origin.cullMode, StateOrigin::Content);
    EXPECT_EQ(resolved.state.depth.test, expected.depth.test);
    EXPECT_EQ(resolved.state.depth.write, expected.depth.write);
    EXPECT_EQ(resolved.state.depth.compare, expected.depth.compare);
    EXPECT_EQ(resolved.state.polygonMode, expected.polygonMode);
    EXPECT_EQ(resolved.state.blend.enabled, expected.blend.enabled);
    EXPECT_EQ(resolved.state.topology, expected.topology);
    EXPECT_TRUE(command.authoredState.depth.has_value()) << "explicit depth is authored on a StateNode";
    EXPECT_EQ(resolved.origin.depth, StateOrigin::Content);

    // The single-valued items: the leaf's own material wins over every ancestor's, the program comes from
    // the nearest ancestor that has one.
    EXPECT_EQ(command.material.get(), own_material.get());
    EXPECT_EQ(command.material.get(), effectiveMaterial(geometry.get()).get());
    EXPECT_EQ(command.program.get(), inner_program.get());
    EXPECT_EQ(command.program.get(), effectiveProgram(geometry.get()).get());

    // With the leaf's own material cleared, the NEAREST ancestor's covers it.
    geometry->setMaterial(nullptr);
    const auto inherited = scene.collectRenderCommands(&cam);
    ASSERT_EQ(inherited.size(), 1u);
    EXPECT_EQ(inherited[0].material.get(), outer_material.get());
    EXPECT_EQ(inherited[0].material.get(), effectiveMaterial(geometry.get()).get());

    // A hidden StateNode hides its whole subtree (the fold must not outlive the culling decision).
    inner->setVisible(false);
    EXPECT_TRUE(scene.collectRenderCommands(&cam).empty());
}

// ============ Backend diagnostics channel ============

/**
 * @brief There is ONE route: the engine owns it, the backend reports through it.
 *
 * A host holds the engine (not the backend instance) and may install the sink before or after the
 * backend, so the engine owns the one route (see Diagnostics.hpp) and hands it to whichever backend it
 * drives - current or later. That is what makes the engine's count the whole truth: what the backend says
 * and what the engine says land in the same counter, and a backend that goes away does not take the
 * count with it.
 */
TEST(DiagnosticsTest, EngineForwardsSinkToCurrentAndFutureBackend)
{
    RenderEngine engine;
    std::vector<RenderDiagnostic> received;
    engine.setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });

    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    engine.setDevice(backend);

    // The backend received the stored sink (installed after setBackend, the
    // engine applies it to the backend it currently holds).
    EXPECT_TRUE(static_cast<bool>(backend->diagnosticSink()));

    // A backend set later gets it too.
    auto later = intrusive_ptr<MockBackend>(new MockBackend());
    engine.setDevice(later);
    EXPECT_TRUE(static_cast<bool>(later->diagnosticSink()));

    // Emitting on the current backend reaches the host through that one route, and the engine reports
    // the count without listening.
    later->emitDiagnostic(DiagnosticSeverity::Warning, DiagnosticCategory::ShaderFallback,
                          u8"synthetic");
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ShaderFallback);
    EXPECT_EQ(received[0].message, u8"synthetic");
    EXPECT_EQ(engine.diagnosticCount(), 1u);
    EXPECT_EQ(later->diagnosticCount(), engine.diagnosticCount())
        << "one route means one number, seen from either end";

    // Clearing the sink stops delivery but not counting, and the count belongs to the ENGINE's route: a
    // backend going away does not un-happen what it said.
    engine.setDiagnosticSink({});
    later->emitDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::GeometryRejected,
                          u8"second");
    EXPECT_EQ(received.size(), 1u);
    EXPECT_EQ(engine.diagnosticCount(), 2u);
    engine.setDevice(nullptr);
    EXPECT_EQ(engine.diagnosticCount(), 2u) << "the route is the engine's, so the count outlives the backend";
}

// ============ Collected-content memo (D27) ============

/**
 * @brief The memo is an optimisation the ENGINE opens, not a new default.
 *
 * A caller that drives collectRenderCommands() itself (a tool, a test, a
 * hand-rolled loop) must keep the plain "walk the tree on every call"
 * behaviour: nothing is memoised until a content frame is announced, so such a
 * caller cannot silently be served a stale list.
 */
TEST(SceneContentCacheTest, CollectingIsNotMemoisedUntilAContentFrameIsOpen)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only"));
    Camera cam;
    setupLookAtCamera(cam);

    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 2u);
    EXPECT_EQ(scene.contentCollectReuseCount(), 0u);
}

/**
 * @brief Inside a content frame the list is memoised per VIEW, and the memo
 * ends with the frame.
 *
 * The key is the camera's view (its projection*view and eye), not the camera
 * object: collecting is a pure function of the view, so a second camera set up
 * identically must share the entry — and a camera with a different view (here a
 * camera further back) gets its own, without evicting the first one.
 */
TEST(SceneContentCacheTest, ContentFrameMemoisesPerViewAndEndsWithTheFrame)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only"));
    Camera cam_a;
    setupLookAtCamera(cam_a);
    Camera cam_same_view;
    setupLookAtCamera(cam_same_view);
    Camera cam_further;
    cam_further.setViewMatrixAsLookAt(Vec3d(0, 0, 9), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam_further.setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    scene.setContentFrame(1);
    EXPECT_EQ(scene.collectRenderCommands(&cam_a).size(), 1u);
    EXPECT_EQ(scene.collectRenderCommands(&cam_a).size(), 1u);
    EXPECT_EQ(scene.collectRenderCommands(&cam_same_view).size(), 1u); // same view: same entry
    EXPECT_EQ(scene.contentCollectCount(), 1u);
    EXPECT_EQ(scene.contentCollectReuseCount(), 2u);

    // A different view is its own entry — and that does not evict the first one
    // (the passes interleave).
    EXPECT_EQ(scene.collectRenderCommands(&cam_further).size(), 1u);
    EXPECT_EQ(scene.collectRenderCommands(&cam_a).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 2u);
    EXPECT_EQ(scene.contentCollectReuseCount(), 3u);

    // Announcing the same frame again is a no-op (every pass may announce it).
    scene.setContentFrame(1);
    EXPECT_EQ(scene.collectRenderCommands(&cam_a).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 2u);

    // The next frame starts empty: the host may have edited the scene between
    // frames (the normal place to edit it), and no entry may outlive its frame.
    scene.setContentFrame(2);
    EXPECT_EQ(scene.collectRenderCommands(&cam_a).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 3u);
}

/**
 * @brief A scene-level edit invalidates the memo, a no-op setter does not, and
 * invalidateContent() is the escape hatch for direct node edits.
 */
TEST(SceneContentCacheTest, SceneLevelEditsInvalidateTheMemo)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only"));
    Camera cam;
    setupLookAtCamera(cam);

    scene.setContentFrame(1);
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 1u);

    scene.setOpacity(0.5f);
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(scene.collectRenderCommands(&cam)[0].opacity, 0.5f);
    EXPECT_EQ(scene.contentCollectCount(), 2u);

    // Re-setting the same value is not a content change: an app that re-applies
    // its settings every frame keeps the memo (and with it the single walk).
    scene.setOpacity(0.5f);
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 2u);
    EXPECT_EQ(scene.contentCollectReuseCount(), 2u);

    // A node edited directly (transform / material / attributes) cannot be
    // observed by the scene: invalidateContent() makes it visible at once. The
    // command opacity is the scene multiplier timed this node's own opacity.
    root->childrenRef()[0]->setOpacity(0.25f);
    scene.invalidateContent();
    EXPECT_EQ(scene.collectRenderCommands(&cam)[0].opacity, 0.125f);
    EXPECT_EQ(scene.contentCollectCount(), 3u);
}

/**
 * @brief Every caller gets its own copy of the memoised list.
 *
 * A pass post-processes the list it is handed (the pass-level program override
 * rewrites every command's program), so the memo must not be shared by
 * reference: one pass' edit would otherwise leak into every later pass.
 */
TEST(SceneContentCacheTest, EachCallerGetsItsOwnCopyOfTheMemoisedList)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only"));
    Camera cam;
    setupLookAtCamera(cam);
    scene.setContentFrame(1);

    auto first = scene.collectRenderCommands(&cam);
    ASSERT_EQ(first.size(), 1u);
    first[0].opacity = 0.25f;

    const auto second = scene.collectRenderCommands(&cam);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].opacity, 1.0f);
    EXPECT_EQ(scene.contentCollectReuseCount(), 1u);
}

// ============ Content layers (which layers a pass draws) ============

/**
 * @brief The mask decides which nodes reach a picture: one AND per node, and a pass is free to draw a part.
 *
 * Layers are the content selection a pass draws with (see Node::setLayers / RenderPass::setLayerMask and
 * `.ai/design/graphics-layers.md`): the mask names layers, a node declares which it is in, and a node that
 * declares none is in kDefaultLayer - so declaring the FEW items of a named layer is enough to keep them out
 * of a pass that draws the rest (a shadow pass drawing casters, two passes showing different parts).
 */
TEST(ContentLayersTest, TheMaskDecidesWhichNodesAreCollected)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"ordinary"));
    auto caster = makeTriangleNode(Vec3d(1, 0, -2), nullptr, u8"caster");
    caster->setLayers(1u << 1);
    root->addChild(caster);

    Camera cam;
    setupLookAtCamera(cam);

    EXPECT_EQ(caster->layers().value_or(0u), 1u << 1) << "setLayers() stores the declaration on the node";

    // The default mask draws EVERY layer, so a scene that says nothing about layers draws as it always did.
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 2u) << "kAllLayers is what a caller naming no mask gets";
    EXPECT_EQ(scene.collectRenderCommands(&cam, kAllLayers).size(), 2u);

    // A selective mask leaves the ordinary content (kDefaultLayer) out, which is what makes it worth having: the
    // few nodes that ARE in the named layer are the ones that had to be declared.
    const auto only_casters = scene.collectRenderCommands(&cam, 1u << 1);
    ASSERT_EQ(only_casters.size(), 1u);
    EXPECT_EQ(only_casters[0].geometry->name(), u8"caster");

    const auto only_ordinary = scene.collectRenderCommands(&cam, kDefaultLayer);
    ASSERT_EQ(only_ordinary.size(), 1u);
    EXPECT_EQ(only_ordinary[0].geometry->name(), u8"ordinary");

    // Several layers are one mask, not a list of calls.
    EXPECT_EQ(scene.collectRenderCommands(&cam, kDefaultLayer | (1u << 1)).size(), 2u);

    // A mask no node intersects selects nothing - the honest answer, not "everything".
    EXPECT_TRUE(scene.collectRenderCommands(&cam, 1u << 20).empty());
}

/**
 * @brief The mask is part of what the frame's memo is keyed on.
 *
 * Two passes may draw the SAME view through the same camera and still want different lists, so a cache keyed
 * on the view alone would serve one pass the other pass' content - a picture that is right except for a few
 * swapped objects, which is the kind of bug nothing points at.
 */
TEST(ContentLayersTest, TheMaskIsPartOfWhatTheMemoIsKeyedOn)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"ordinary"));
    Camera cam;
    setupLookAtCamera(cam);
    scene.setContentFrame(1);

    // One mask, asked twice: the frame's memo answers the second time (one walk per view per frame).
    EXPECT_EQ(scene.collectRenderCommands(&cam, kDefaultLayer).size(), 1u);
    EXPECT_EQ(scene.collectRenderCommands(&cam, kDefaultLayer).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 1u);
    EXPECT_EQ(scene.contentCollectReuseCount(), 1u);

    // A DIFFERENT mask over the same view is a different list: the walk happens again, for its own entry.
    EXPECT_TRUE(scene.collectRenderCommands(&cam, 1u << 20).empty());
    EXPECT_EQ(scene.contentCollectCount(), 2u) << "one walk per (view, mask)";
    EXPECT_EQ(scene.contentCollectReuseCount(), 1u);

    // And each key keeps its own answer afterwards, in any order.
    EXPECT_TRUE(scene.collectRenderCommands(&cam, 1u << 20).empty());
    EXPECT_EQ(scene.collectRenderCommands(&cam, kDefaultLayer).size(), 1u);
    EXPECT_EQ(scene.contentCollectCount(), 2u);
    EXPECT_EQ(scene.contentCollectReuseCount(), 3u);
}

/**
 * @brief A declaration covers the subtree, the nearest one wins, and withdrawing it inherits again.
 *
 * The same rule the shading state follows (see StateNode / Scene.cpp's InheritedState): one fold, computed on
 * the way down, so a subtree can be re-declared in one call and un-declared in another.
 */
TEST(ContentLayersTest, TheNearestDeclarationCoversItsSubtree)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    auto  group = intrusive_ptr<Group>(new Group());
    group->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"inherited"));
    auto overridden = makeTriangleNode(Vec3d(1, 0, -2), nullptr, u8"redeclared");
    overridden->setLayers(1u << 2);
    group->addChild(overridden);
    group->setLayers(1u << 1);
    root->addChild(group);
    root->addChild(makeTriangleNode(Vec3d(-1, 0, -2), nullptr, u8"outside"));

    Camera cam;
    setupLookAtCamera(cam);

    // The parent's declaration covers the child that says nothing; the child that declares its own replaces it.
    EXPECT_EQ(scene.collectRenderCommands(&cam).size(), 3u) << "the default mask still draws all of them";
    auto in_parent_layer = scene.collectRenderCommands(&cam, 1u << 1);
    ASSERT_EQ(in_parent_layer.size(), 1u);
    EXPECT_EQ(in_parent_layer[0].geometry->name(), u8"inherited");
    auto in_child_layer = scene.collectRenderCommands(&cam, 1u << 2);
    ASSERT_EQ(in_child_layer.size(), 1u);
    EXPECT_EQ(in_child_layer[0].geometry->name(), u8"redeclared");
    auto ordinary = scene.collectRenderCommands(&cam, kDefaultLayer);
    ASSERT_EQ(ordinary.size(), 1u);
    EXPECT_EQ(ordinary[0].geometry->name(), u8"outside") << "a node outside the declared subtree is untouched";

    // Withdrawing the declaration puts the subtree back to what it inherited (here: the default layer).
    group->clearLayers();
    EXPECT_EQ(group->layers(), std::nullopt);
    auto after_withdrawal = scene.collectRenderCommands(&cam, kDefaultLayer);
    EXPECT_EQ(after_withdrawal.size(), 2u) << "the group's own child is ordinary content again";
    EXPECT_TRUE(scene.collectRenderCommands(&cam, 1u << 1).empty()) << "and nothing is left in that layer";
}

/**
 * @brief Visibility and layers are two questions, and neither answers the other.
 *
 * setVisible(false) is "temporarily off" (a host toggle, a LOD swap); a layer is "which part of the content
 * this is" (a property of the content, stated once). A pass draws what is visible AND in its layers.
 */
TEST(ContentLayersTest, VisibilityAndLayersAreTwoQuestions)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    auto  node = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only");
    node->setLayers(1u << 1);
    root->addChild(node);

    Camera cam;
    setupLookAtCamera(cam);

    EXPECT_EQ(scene.collectRenderCommands(&cam, 1u << 1).size(), 1u);

    node->setVisible(false);
    EXPECT_TRUE(scene.collectRenderCommands(&cam, 1u << 1).empty()) << "hidden wins over the layer";

    node->setVisible(true);
    EXPECT_EQ(scene.collectRenderCommands(&cam, 1u << 1).size(), 1u) << "the layer survived the toggle";
    EXPECT_TRUE(scene.collectRenderCommands(&cam, kDefaultLayer).empty()) << "and the declaration still holds";
}

/**
 * @brief The declaration carries the mask onto the pass it builds, and the plan says so.
 *
 * The mask travels the same way every other pass fact does: `PassDesc` -> the pass the engine registered ->
 * the compiled frame's row. That is what lets a host read a frame and see which layers each pass draws
 * instead of having to remember how the chain was built.
 */
TEST(ContentLayersTest, TheDeclarationCarriesTheMaskOntoThePassAndIntoThePlan)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());

    RenderPass direct;
    direct.setName(u8"direct");
    EXPECT_EQ(direct.layerMask(), kAllLayers) << "an unstated mask is every layer";
    direct.setLayerMask(0x3u);
    EXPECT_EQ(direct.layerMask(), 0x3u);

    PassDesc desc;
    desc.name       = u8"declared";
    desc.layer_mask = 0x9u;
    engine->addPass(desc);

    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 1u);
    EXPECT_EQ(plan.passes[0].pass->layerMask(), 0x9u) << "the descriptor's mask is the pass' mask";
    EXPECT_EQ(plan.passes[0].layer_mask, 0x9u) << "and the frame's row states it";

    // The dump carries it for a pass that says something other than the default, and stays quiet for the rest.
    const std::string one_pass = plan.describe().as_std_str();
    EXPECT_NE(one_pass.find("layers=0x9"), std::string::npos) << one_pass;
    engine->clearPasses();
    engine->addPass(RenderPassPtr(new RenderPass()), 0);
    EXPECT_EQ(engine->framePlan().describe().as_std_str().find("layers="), std::string::npos)
        << "a pass drawing every layer says nothing about them";
}

/**
 * @brief A recipe states its content selection once, and the chain's content passes all draw it.
 *
 * The shadow pass is why this is a property of the RECIPE and not of each pass: what the picture does not show
 * must not cast either, and a chain where one content pass forgot the mask would draw a shadow of content the
 * picture leaves out.
 */
TEST(ContentLayersTest, ARecipeStampsItsMaskOnEveryContentPassButNotOnTheOverlays)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*content)->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only"));
    auto cam = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*cam);

    PipelineOptions options;
    options.layer_mask = 1u << 1;
    options.gizmo.source_camera = cam.get();

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(options);
    ASSERT_NE(pipeline, nullptr);

    std::size_t content_passes = 0;
    std::size_t overlay_passes = 0;
    for (const PassPlan& pass : engine->framePlan().passes) {
        if (pass.pass == pipeline->gizmoPass()) {
            EXPECT_EQ(pass.layer_mask, kAllLayers) << "an overlay is UI: the content selection must not hide it";
            ++overlay_passes;
            continue;
        }
        if (pass.content == content.get()) {
            EXPECT_EQ(pass.layer_mask, 1u << 1) << "a content pass of the chain draws the recipe's layers";
            ++content_passes;
        }
    }
    EXPECT_EQ(content_passes, 1u) << "the forward path's window pass";
    EXPECT_EQ(overlay_passes, 1u) << "the gizmo overlay the recipe asked for";
}

/**
 * @brief A content pass whose mask selects nothing is legal, draws nothing, and says so once.
 *
 * The declaration is the host's to make (0 selects no layer), so the frame runs it and issues no command -
 * "it ran and had nothing" rather than a silent empty picture. What it does NOT do is repeat itself: the frame
 * plan's problems are reported once per episode, so a steady frame says nothing.
 */
TEST(ContentLayersTest, APassThatSelectsNoLayerDrawsNothingAndSaysSoOnce)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto scene = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only"));
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setName(u8"blind");
    pass->setCamera(camera);
    pass->setLayerMask(0u);
    engine->addPass(pass, scene, 0);

    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::ContentSkipped), 0u) << "nothing ran yet";

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::ContentSkipped), 1u);
    ASSERT_FALSE(backend->render_command_counts.empty());
    EXPECT_EQ(backend->render_command_counts.back(), 0u)
        << "the pass ran and issued a drawing call with no command in it";

    // The same declaration, the same frame: the episode has been said.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::ContentSkipped), 1u) << "one condition, one report";

    ASSERT_EQ(engine->framePlan().passes.size(), 1u);
    EXPECT_EQ(engine->framePlan().passes[0].layer_mask, 0u) << "the plan still states the declaration";
}

// ============ Draw order (where a subtree's draws sit inside one pass) ============

/**
 * @brief The declared draw order is the outer key of the sort: higher is drawn later, whatever the tree says.
 *
 * With nothing declared the picture is the one this collection has always produced (the traversal order, kept
 * by a stable sort), and a declaration only says WHERE in that list a subtree's draws belong - which is how
 * content that must land on top of the rest (an outline, a decal, a highlight) is expressed without a pass of
 * its own.
 */
TEST(DrawOrderTest, TheDeclaredOrderIsTheOuterKeyOfTheSort)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    // All three at the SAME depth, so the declaration is the only thing that can move them.
    auto first  = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"first");
    auto second = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"second");
    auto third  = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"third");
    root->addChild(first);
    root->addChild(second);
    root->addChild(third);

    Camera cam;
    setupLookAtCamera(cam);
    const auto order = [](const std::vector<RenderCommand>& commands) {
        std::string text;
        for (const RenderCommand& command : commands) {
            if (!text.empty()) {
                text += ",";
            }
            text += command.geometry->name().as_std_str();
        }
        return text;
    };

    EXPECT_EQ(order(scene.collectRenderCommands(&cam)), "first,second,third")
        << "nobody declared one: the traversal order decides, and the sort is stable";

    // A higher order is drawn LATER - the subtree added first comes out last.
    first->setDrawOrder(2);
    EXPECT_EQ(order(scene.collectRenderCommands(&cam)), "second,third,first");
    EXPECT_EQ(scene.collectRenderCommands(&cam).back().drawOrder, 2) << "the command carries the resolved order";

    // A lower one is drawn EARLIER (negative orders are as legal as positive ones).
    third->setDrawOrder(-1);
    EXPECT_EQ(order(scene.collectRenderCommands(&cam)), "third,second,first");

    // Withdrawing the declarations puts the picture back where it started.
    first->clearDrawOrder();
    third->clearDrawOrder();
    EXPECT_EQ(order(scene.collectRenderCommands(&cam)), "first,second,third");
}

/**
 * @brief A declared order leapfrogs the batches, because it is the OUTER key.
 *
 * The collection draws opaque content before transparent content (transparent needs painter's order), so an
 * opaque draw could never land on top of a transparent one - which is exactly what a highlight or an outline
 * must do. INSIDE one order the old picture still holds; the leapfrog is what the declaration buys.
 */
TEST(DrawOrderTest, ADrawOrderLeapfrogsTheTransparentBatch)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    auto  plain = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"plain");
    auto  glass = makeTriangleNode(Vec3d(0, 0, -4), nullptr, u8"glass");
    glass->setOpacity(0.5f);
    auto highlight = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"highlight");
    root->addChild(plain);
    root->addChild(glass);
    root->addChild(highlight);

    Camera cam;
    setupLookAtCamera(cam);
    const auto order = [](const std::vector<RenderCommand>& commands) {
        std::string text;
        for (const RenderCommand& command : commands) {
            if (!text.empty()) {
                text += ",";
            }
            text += command.geometry->name().as_std_str();
        }
        return text;
    };

    EXPECT_EQ(order(scene.collectRenderCommands(&cam)), "plain,highlight,glass")
        << "no declarations: opaque (near to far), then transparent";

    highlight->setDrawOrder(1);
    EXPECT_EQ(order(scene.collectRenderCommands(&cam)), "plain,glass,highlight")
        << "the highlight is opaque and still lands AFTER the transparent draw - that is the point of the key";

    // Inside one order nothing changed: the transparent draw is still last among its own order.
    glass->setDrawOrder(0);
    EXPECT_EQ(order(scene.collectRenderCommands(&cam)), "plain,glass,highlight");
}

/**
 * @brief The declaration covers the subtree, the nearest one wins, and withdrawing it inherits again.
 *
 * The same fold the layers and the shading state follow (see Scene.cpp's InheritedState): one rule, one
 * implementation, so a group can be moved as a unit and a child can still say otherwise.
 */
TEST(DrawOrderTest, TheNearestDeclarationCoversItsSubtree)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    auto  group = intrusive_ptr<Group>(new Group());
    auto  inherited = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"inherited");
    auto  overridden = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"overridden");
    group->addChild(inherited);
    group->addChild(overridden);
    root->addChild(group);

    Camera cam;
    setupLookAtCamera(cam);
    const auto resolved_order = [&scene, &cam](vn::raw_ptr<const Geometry> geometry) {
        for (const RenderCommand& command : scene.collectRenderCommands(&cam)) {
            if (command.geometry.get() == geometry) {
                return command.drawOrder;
            }
        }
        return -999;
    };
    const auto geometry_of = [](const intrusive_ptr<MatrixTransform>& node) {
        vn::raw_ptr<const Geometry> geometry = nullptr;
        for (const auto& child : node->childrenRef()) {
            if (const auto* leaf = dynamic_cast<const Geometry*>(child.get())) {
                geometry = leaf;
            }
        }
        return geometry;
    };

    group->setDrawOrder(3);
    EXPECT_EQ(group->drawOrder().value_or(0), 3) << "the declaration is on the group";
    EXPECT_EQ(resolved_order(geometry_of(inherited)), 3) << "the child that says nothing inherits it";
    EXPECT_EQ(resolved_order(geometry_of(overridden)), 3);

    overridden->setDrawOrder(-2);
    EXPECT_EQ(resolved_order(geometry_of(inherited)), 3) << "the group is untouched by its child's declaration";
    EXPECT_EQ(resolved_order(geometry_of(overridden)), -2) << "the nearest declaration wins";

    overridden->clearDrawOrder();
    EXPECT_EQ(resolved_order(geometry_of(overridden)), 3) << "withdrawn: it inherits again";
    EXPECT_EQ(overridden->drawOrder(), std::nullopt);

    group->clearDrawOrder();
    EXPECT_EQ(resolved_order(geometry_of(inherited)), 0) << "nothing declared anywhere: the default";
}

/**
 * @brief The declaration also works ON the drawable, which is where a single object's order belongs.
 *
 * The value is a `Node`'s, not a group's: a leaf that declares one is drawn where it says, and being inside a
 * group that declared another one is not a contradiction - the nearest declaration wins, so the leaf's own is
 * the answer. (This is the shape a highlight uses: one geometry, not a subtree.)
 */
TEST(DrawOrderTest, TheDeclarationAppliesAtTheDrawableToo)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    auto  plain = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"plain");
    auto  marked = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"marked");
    // The geometry is the drawn node; the helper's MatrixTransform is its parent. Declaring on the LEAF itself
    // is what this pins (the cases above declare on a parent and read the inherited value).
    vn::raw_ptr<const Geometry> marked_geometry = nullptr;
    for (const auto& child : marked->childrenRef()) {
        if (const auto* leaf = dynamic_cast<const Geometry*>(child.get())) {
            marked_geometry = leaf;
        }
    }
    ASSERT_NE(marked_geometry, nullptr);
    const_cast<Geometry*>(marked_geometry)->setDrawOrder(5);

    root->addChild(plain);
    root->addChild(marked);

    Camera cam;
    setupLookAtCamera(cam);
    const auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].geometry->name(), u8"plain") << "the marked one is drawn later, by its own declaration";
    EXPECT_EQ(commands[1].geometry->name(), u8"marked");
    EXPECT_EQ(commands[1].drawOrder, 5) << "the leaf's own declaration is the command's order";
}

/**
 * @brief Several passes over one scene share one walk per view per frame.
 *
 * This is the D27 fix: a multi-pass pipeline that draws the same scene through
 * the same camera several times per frame (G-buffer producer, its lighting
 * consumer, ...) used to walk the whole tree once per pass; now it walks once
 * per VIEW (the camera the pass uses), and each frame walks again because the
 * host edits the scene between frames.
 */
TEST(RenderEngineTest, PassesSharingSceneAndCameraCollectOncePerCameraPerFrame)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto scene = intrusive_ptr<Scene>(new Scene());
    auto root  = setIdentityRoot(*scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"only"));
    auto cam_a = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*cam_a);
    // A genuinely different view (a shadow / reflection camera, say): an
    // identical view would legitimately share the first camera's entry.
    auto cam_b = intrusive_ptr<Camera>(new Camera());
    cam_b->setViewMatrixAsLookAt(Vec3d(0, 0, 9), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam_b->setProjectionMatrixAsPerspective(60.0, 0.1, 1000.0);

    auto pass_a = intrusive_ptr<RenderPass>(new RenderPass());
    pass_a->setCamera(cam_a);
    auto pass_b = intrusive_ptr<RenderPass>(new RenderPass());
    pass_b->setCamera(cam_a);
    auto pass_c = intrusive_ptr<RenderPass>(new RenderPass());
    pass_c->setCamera(cam_b);
    engine->addPass(pass_a, scene, 0);
    engine->addPass(pass_b, scene, 1);
    engine->addPass(pass_c, scene, 2);

    // Deltas: the pre-frame warm-up also executes passes and collects.
    const auto walks_before  = scene->contentCollectCount();
    const auto reuses_before = scene->contentCollectReuseCount();

    // Each pass is described ONCE per frame: the compile describes it into the frame's value and the run announces
    // that value (see RenderEngine::buildFrameValue) - so the reuses are exactly the passes that shared the walk
    // this frame: pass_b's.
    engine->frame(0.016);
    EXPECT_EQ(scene->contentCollectCount() - walks_before, 2u);       // one per view
    EXPECT_EQ(scene->contentCollectReuseCount() - reuses_before, 1u); // pass_b reused pass_a's

    engine->frame(0.016);
    EXPECT_EQ(scene->contentCollectCount() - walks_before, 4u);       // the next frame walks again
    EXPECT_EQ(scene->contentCollectReuseCount() - reuses_before, 2u);
}

TEST(RenderEngineTest, OneCameraCullsWithItsOwnShapeInEveryRectangle)
{
    // ONE camera, TWO rectangles: the same eye, the same fov - and, because a camera holds the shape of its
    // picture (see Camera), ONE shape. The rectangles differ, so one of the two pictures is drawn in a shape its
    // camera does not have (a stretched picture - reported, see the engine's mismatch diagnostic); what neither
    // rectangle buys is a different frustum or a walk of its own: the culling and the walk follow the CAMERA.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto scene = intrusive_ptr<Scene>(new Scene());
    auto root  = setIdentityRoot(*scene);
    root->addChild(makeTriangleNode(Vec3d(7, 0, -3), nullptr, u8"off-to-the-side"));
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);  // eye (0,0,5), vertical fov 60, shape 1.0: about +-4.6 wide at that distance

    auto wide = intrusive_ptr<RenderPass>(new RenderPass());
    wide->setCamera(camera);
    wide->setViewport(0, 0, 1600, 600);  // shape 2.67
    auto square = intrusive_ptr<RenderPass>(new RenderPass());
    square->setCamera(camera);
    square->setViewport(0, 0, 600, 600);  // shape 1.0
    engine->addPass(wide, scene, 0);
    engine->addPass(square, scene, 1);

    const auto        walks_before  = scene->contentCollectCount();
    const auto        reuses_before = scene->contentCollectReuseCount();
    const std::size_t calls_before  = backend->render_command_counts.size();
    engine->frame(0.016);

    // The wide rectangle does not widen the frustum: both passes cull with the camera's own (square) shape, which
    // puts the object off to the side of both.
    ASSERT_EQ(backend->render_command_counts.size() - calls_before, 2u) << "one drawing call per pass";
    EXPECT_EQ(backend->render_command_counts[calls_before], 0u)
        << "the wide rectangle does not hand the camera a wider shape";
    EXPECT_EQ(backend->render_command_counts[calls_before + 1], 0u);
    EXPECT_EQ(scene->contentCollectCount() - walks_before, 1u)
        << "one walk for the frame: one camera, one shape, however many rectangles it fills";
    // One describe per pass per frame (the frame runs from the value the compile built): the second pass is served
    // by the memo, and nothing describes anything twice.
    EXPECT_EQ(scene->contentCollectReuseCount() - reuses_before, 1u);

    // Giving the camera a wider shape gives BOTH rectangles the wider view - and the new shape its own walk.
    camera->setAspect(8.0 / 3.0);
    const std::size_t walks_before_wide = scene->contentCollectCount();
    engine->frame(0.032);

    ASSERT_EQ(backend->render_command_counts.size() - calls_before, 4u);
    EXPECT_EQ(backend->render_command_counts[calls_before + 2], 1u) << "the wide shape holds the off-centre object";
    EXPECT_EQ(backend->render_command_counts[calls_before + 3], 1u)
        << "and so does the square rectangle the camera was given it for - the picture is stretched, not re-culled";
    EXPECT_EQ(scene->contentCollectCount() - walks_before_wide, 1u) << "one walk per SHAPE the camera has been given";
}

/**
 * @brief The frame's value is built from the state the frame is ABOUT to draw, per-frame hooks included.
 *
 * A pass that updates its own content every frame does it through the hook its declaration carries
 * (PassDesc::before_draw - the FPS readout, the mirrored gizmo), and the compile runs it before it asks the pass
 * to describe itself: the value must carry the CONTENT OF THIS FRAME, not the previous one's. That is the
 * ordering a host cannot see on the wire (both orders announce the same calls, one frame apart), which is why it
 * is pinned here: the value is rebuilt from scratch every frame (the engine's own value is per frame), so "the
 * compile describes after the hook" is the only thing keeping a readout from being a frame behind.
 */
TEST(RenderEngineTest, TheFramesValueIsBuiltAfterThePerFrameHooks)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto scene  = intrusive_ptr<Scene>(new Scene());
    auto root   = setIdentityRoot(*scene);
    root->addChild(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setName(u8"hud");
    pass->setCamera(camera);

    int  hook_runs        = 0;
    bool content_replaced = false;
    PassDesc desc;
    desc.name       = u8"hud";
    desc.camera     = camera;
    desc.content    = scene;
    desc.before_draw = [&] {
        ++hook_runs;
        if (!content_replaced) {
            // Once, the way an overlay rewrites what it shows: the frame's value has to describe the version the
            // hook left behind.
            content_replaced = true;
            root->addChild(makeTriangleNode(Vec3d(0, 0, -6), nullptr, u8"readout"));
        }
    };
    ASSERT_NE(engine->addPass(desc), nullptr);

    engine->frame(0.016);
    EXPECT_EQ(hook_runs, 1) << "the hook runs once per frame, exactly where the frame used to run it";
    const FrameDescription& first = engine->frameDescription();
    ASSERT_EQ(first.passes.size(), 1u);
    ASSERT_TRUE(first.passes[0].description.has_value());
    ASSERT_EQ(first.passes[0].description->draws.size(), 1u);
    EXPECT_EQ(first.passes[0].description->draws[0].commands->size(), 2u)
        << "the value carries what the pass draws after the hook ran this frame, not before";

    // A frame where the hook changes nothing describes the same content: the value is rebuilt per frame, never
    // edited in place.
    engine->frame(0.016);
    EXPECT_EQ(hook_runs, 2);
    ASSERT_TRUE(engine->frameDescription().passes[0].description.has_value());
    EXPECT_EQ(engine->frameDescription().passes[0].description->draws[0].commands->size(), 2u);
}

/**
 * @brief The frame's value is exactly what the frame ran: every pass, every draw, and the facts around them.
 *
 * The frame is a value inside the engine (compileFrame decides the order, the wiring and the problems), and this
 * is that value at the boundary - assembled BEFORE anything is dispatched, which is what the camera holding the
 * shape of its own picture made possible (a description asks the backend nothing). The criterion is the strong
 * one: what the value says and what the backend's own walk received have to line up item for item, so a field the
 * value does not carry (or a pass it does not have) cannot hide.
 */
TEST(RenderEngineTest, TheFrameValueIsEveryPassAndEveryDrawTheFrameRan)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto scene = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    scene->addLight(Light::createAmbient());
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    // ① a scene pass into an off-screen target, with a sub-viewport and a program override.
    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(320, 180);
    auto first = intrusive_ptr<RenderPass>(new RenderPass());
    first->setName(u8"scene");
    first->setCamera(camera);
    first->setRenderTarget(target);
    first->setViewport(0, 0, 160, 90);
    auto override_program = vn::graphics::forwardProgram();
    first->setProgramOverride(override_program);
    // ② a screen pass sampling ①'s target.
    auto second = intrusive_ptr<ScreenPass>(new ScreenPass());
    second->setName(u8"composite");
    second->setCamera(camera);
    second->setProgram(vn::graphics::screenCopyProgram());
    second->addInputTarget(target);
    // ③ a pass that is enabled and has content but no camera: it issues no DRAW, and still has an entry.
    auto third = intrusive_ptr<RenderPass>(new RenderPass());
    third->setName(u8"nofview");
    third->setRenderTarget(target);
    // ④ a pass with no content at all: it issues NOTHING (not even a target) and says so with no description.
    auto fourth = intrusive_ptr<RenderPass>(new RenderPass());
    fourth->setName(u8"noscene");
    fourth->setCamera(camera);
    // ⑤ a disabled pass: it does not run, so it is not in the value at all.
    auto fifth = intrusive_ptr<RenderPass>(new RenderPass());
    fifth->setName(u8"disabled");
    fifth->setCamera(camera);
    fifth->setEnabled(false);

    engine->addPass(first, scene, 0);
    engine->addPass(second, scene, 1);
    engine->addPass(third, scene, 2);
    engine->addPass(fourth, nullptr, 3);
    engine->addPass(fifth, scene, 4);
    engine->frame(0.016);

    const FrameDescription& value = engine->frameDescription();
    ASSERT_EQ(value.passes.size(), 4u) << "the four enabled passes; the disabled one does not run";

    // THE PASSES, in run order and with the identity the backend was handed (its per-pass state key).
    ASSERT_EQ(backend->pass_entries.size(), 4u);
    EXPECT_EQ(backend->frames_executed, 1) << "the whole frame arrives in ONE call (see RenderFrameExecutor::executeFrame)";
    EXPECT_EQ(backend->entries_closed, backend->entries) << "every entry is walked to its end";
    for (std::size_t index = 0; index < value.passes.size(); ++index) {
        EXPECT_EQ(value.passes[index].pass, backend->pass_entries[index])
            << "entry " << index << " is the pass the backend's walk reached";
    }

    // ① the scene pass: its target, its viewport, its state and its one draw, with the rewritten commands.
    const FrameDescription::Pass& scene_entry = value.passes[0];
    ASSERT_TRUE(scene_entry.description.has_value());
    const PassDescription& scene_pass = *scene_entry.description;
    EXPECT_EQ(scene_pass.target, target.get());
    ASSERT_TRUE(scene_pass.viewport.has_value());
    EXPECT_EQ(scene_pass.viewport->width, 160);
    ASSERT_EQ(scene_pass.draws.size(), 1u);
    ASSERT_EQ(backend->render_cameras.size(), 1u);
    EXPECT_EQ(scene_pass.draws[0].camera, backend->render_cameras[0]);
    EXPECT_EQ(scene_pass.draws[0].commands->size(), backend->render_command_counts[0]);
    EXPECT_EQ(scene_pass.draws[0].commands->front().program.get(), override_program.get())
        << "the program the override put in the commands is the program the pass drew with";

    // ② the screen pass: a screen draw with its source and program, and the input it resolved.
    const FrameDescription::Pass& screen_entry = value.passes[1];
    ASSERT_EQ(screen_entry.inputs.size(), 1u);
    EXPECT_EQ(screen_entry.inputs[0], target.get()) << "what its declared input resolved to this frame";
    ASSERT_TRUE(screen_entry.description.has_value());
    const PassDescription& screen_pass = *screen_entry.description;
    ASSERT_EQ(screen_pass.draws.size(), 1u);
    EXPECT_EQ(screen_pass.draws[0].kind, DrawDescription::Kind::Screen);
    EXPECT_EQ(screen_pass.draws[0].source, backend->last_program_source);
    EXPECT_EQ(screen_pass.draws[0].program.get(), backend->last_program);
    EXPECT_EQ(backend->program_draws, 1);

    // ③ a pass with no camera: an entry that says where and with what, and draws nothing.
    const FrameDescription::Pass& no_view = value.passes[2];
    ASSERT_TRUE(no_view.description.has_value());
    EXPECT_EQ(no_view.description->target, target.get());
    EXPECT_TRUE(no_view.description->draws.empty()) << "no camera ⇒ no draw, not a draw of nothing";
    EXPECT_EQ(backend->render_cameras.size(), 1u) << "and the backend was handed no second drawing call";

    // ④ a pass with no content: an entry with NO description - nothing at all was announced for it, which is a
    // different fact from ③ (where the target and the state were announced).
    EXPECT_FALSE(value.passes[3].description.has_value())
        << "a scene pass with no scene issues nothing at all, and the value says exactly that";

    // The number of drawing calls the value claims is the number the backend was handed.
    std::size_t value_draws = 0;
    for (const FrameDescription::Pass& entry : value.passes) {
        value_draws += entry.description.has_value() ? entry.description->draws.size() : 0u;
    }
    EXPECT_EQ(value_draws, backend->render_calls + backend->program_draws);
}

/**
 * @brief The engine runs the frame's VALUE: each pass is described once per frame, and never asked to execute.
 *
 * THE CONTRACT CHANGE OF A VALUE-DRIVEN FRAME, named. The compile describes every enabled pass once
 * (RenderEngine::buildFrameValue) and the run announces what came out, so a pass that overrides execute() and
 * expects the engine to call it does not run at all: what it issues belongs in describe(), and anything that has
 * to happen before the frame describes it belongs in the declaration's hook (PassDesc::before_draw). Pinned here
 * because the other half of it - "described once" - is what keeps the content walk memo honest (one walk per view
 * per frame, see PassesSharingSceneAndCameraCollectOncePerCameraPerFrame).
 */
TEST(RenderEngineTest, TheEngineRunsTheFrameValueAndNeverCallsExecute)
{
    class CountingPass : public RenderPass
    {
      public:
        int  describes = 0;
        int  executes  = 0;
        bool describe(vn::raw_ptr<Scene> scene, PassDescription& out) override
        {
            ++describes;
            return RenderPass::describe(scene, out);
        }
        void execute(vn::raw_ptr<Scene>, vn::raw_ptr<RenderBackend>,
                     vn::raw_ptr<PresentTarget>) override
        {
            ++executes;
        }
    };

    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto scene  = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto pass = intrusive_ptr<CountingPass>(new CountingPass());
    pass->setName(u8"counted");
    pass->setCamera(camera);
    engine->addPass(pass, scene, 0);

    const int describes_before = pass->describes;
    const int executes_before  = pass->executes;

    engine->frame(0.016);
    engine->frame(0.016);

    EXPECT_EQ(pass->describes - describes_before, 2) << "once per frame, by the compile";
    EXPECT_EQ(pass->executes - executes_before, 0) << "the engine announces the value instead";
    // And the pass really did reach the backend - from the value, with its camera and its content.
    ASSERT_EQ(backend->pass_entries.size(), 2u) << "one entry per frame";
    EXPECT_EQ(backend->render_calls, 2);
    ASSERT_EQ(backend->render_cameras.size(), 2u);
    EXPECT_EQ(backend->render_cameras[1], camera.get());
    EXPECT_EQ(backend->render_command_counts[1], 1u);
}

/**
 * @brief A camera whose picture shape is not the rectangle its pass draws into is reported, not silently stretched.
 *
 * ONE CAMERA, ONE PICTURE SHAPE (see Camera): the projection carries the camera's shape, so the rectangle a pass
 * draws into can disagree with it — a shared camera between two panes, a camera nobody gave a shape, a window that
 * the camera was never told about. Each of the two halves is legal on its own (the backend is handed a rectangle
 * and a camera snapshot), so only the engine can see it, and what it looks like is a STRETCHED picture. Reported
 * once per pass: a host dragging a pane changes the rectangle every frame while the mistake stays the same one.
 */
TEST(RenderEngineTest, ACameraWhoseShapeIsNotThePassRectangleIsReportedOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto scene = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);   // shape 1.0
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setName(u8"window");
    pass->setCamera(camera);
    engine->addPass(pass, scene, 0);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));
    area->resize(800, 600);       // the window pass' rectangle: shape 4:3

    engine->frame(0.016);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"window"), vn::String::npos) << "the pass is named";
    EXPECT_NE(received[0].message.find(u8"800x600"), vn::String::npos) << "the rectangle is stated";
    EXPECT_NE(received[0].message.find(u8"1:1"), vn::String::npos) << "and the picture the camera projects for";

    // The same mismatch: said once, not once per frame - even when a resize moves the rectangle, which is the same
    // mistake in a new size, not a new mistake.
    engine->frame(0.016);
    area->resize(1600, 1200);
    engine->frame(0.016);
    EXPECT_EQ(received.size(), 1u);

    // The camera is given the rectangle's shape: there is nothing left to report, and the episode ends here.
    camera->setAspect(1600.0 / 1200.0);
    engine->frame(0.016);
    EXPECT_EQ(received.size(), 1u);

    // Wrong again: a mismatch that was fixed and came back is a NEW episode, not one the engine remembers forever.
    camera->setAspect(1.0);
    engine->frame(0.016);
    EXPECT_EQ(received.size(), 2u) << "a mismatch that was fixed and came back is reported again";

    // The pass is taken out of the frame, and comes back with the same mistake: a new episode, since the report is
    // about a pass IN a frame (there was no picture being stretched while it was gone).
    engine->removePass(pass.get());
    engine->frame(0.016);
    EXPECT_EQ(received.size(), 2u) << "a pass that is not in the frame draws no picture to stretch";
    engine->addPass(pass, scene, 0);
    engine->frame(0.016);
    EXPECT_EQ(received.size(), 3u) << "and the same pass back in the frame is a new episode";
}

/**
 * @brief A pass' SUB-VIEWPORT is what its camera is measured against, and a screen pass is not measured at all.
 *
 * The rectangle a pass draws into is its viewport when it named one (the whole target otherwise), which is why a
 * picture-in-picture camera is shaped for the box and not for the surface it sits in. A ScreenPass is the one kind
 * that is never measured: its full-screen triangle is built in clip space and reads no projection (see
 * BuiltinShaders::fullscreenVertexProgram), so its camera states the view the shading is computed in - the box it
 * composites into cannot stretch it, whatever shape either of them has.
 */
TEST(RenderEngineTest, TheRectangleIsThePassViewportAndAScreenPassIsNotMeasured)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto scene = intrusive_ptr<Scene>(new Scene());
    setIdentityRoot(*scene)->addChild(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    // A corner preview: a sub-rectangle of the window, projected for its own shape - which is what it is given.
    auto preview = intrusive_ptr<RenderPass>(new RenderPass());
    preview->setName(u8"preview");
    preview->setCamera(camera);
    preview->setViewport(600, 0, 400, 400);   // shape 1.0, the camera's own
    engine->addPass(preview, scene, 0);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));
    area->resize(800, 600);                   // the SURFACE is 4:3, the pass draws into a square

    engine->frame(0.016);
    EXPECT_EQ(received.size(), 0u) << "the camera is shaped for the box it draws into, not for the surface";

    // The same camera in the full window (4:3, its own picture 1:1): that IS a stretched picture.
    auto full = intrusive_ptr<RenderPass>(new RenderPass());
    full->setName(u8"full");
    full->setCamera(camera);
    engine->addPass(full, scene, 1);
    engine->frame(0.016);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_NE(received[0].message.find(u8"full"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"800x600"), vn::String::npos) << "the whole surface, no viewport named";

    // A ScreenPass composites with a full-screen program: its camera states the view the shading is computed in,
    // and the rectangle it lands in cannot stretch what a clip-space triangle draws. Its input resolves, so it
    // really is a drawing pass in this frame - and it draws into the whole (4:3) window with a 1:1 camera.
    auto source = RenderTargetPtr(new RenderTarget());
    source->setSize(400, 400);
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer");
    producer->setCamera(camera);
    producer->setRenderTarget(source);
    producer->setOutputName(u8"GBuffer");
    engine->addPass(producer, scene, 2);

    auto screen = intrusive_ptr<ScreenPass>(new ScreenPass());
    screen->setName(u8"copy");
    screen->setCamera(camera);
    screen->setProgram(vn::graphics::screenCopyProgram());
    screen->addInputName(u8"GBuffer");
    engine->addPass(screen, scene, 3);

    engine->frame(0.016);
    ASSERT_EQ(backend->program_draws, 1) << "the screen pass drew (its input resolved)";
    EXPECT_EQ(received.size(), 1u) << "a full-screen pass is not measured against its rectangle";
}

/**
 * @brief An unresolved declared pass input is reported, not silently empty.
 *
 * A ScreenPass keeps the first input that resolves and draws nothing when there
 * is none, so a pass whose producer did not run this frame (disabled, removed,
 * renamed, or ordered after it) used to lose its content without a word — the
 * wiring is the engine's job, and only the engine can see it (the backend just
 * gets a pass with no source). Reported once per pass, and re-armed when the
 * input resolves again.
 */
TEST(RenderEngineTest, UnresolvedDeclaredInputIsReportedOnceAndRearmed)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    // A consumer with no producer for "GBuffer".
    auto consumer = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"light");
    consumer->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    consumer->setProgram(vn::graphics::screenCopyProgram());
    consumer->addInputName(u8"GBuffer");
    engine->addPass(consumer, 0);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"GBuffer"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"light"), vn::String::npos);

    // Still missing: reported once, not once per frame.
    engine->frame(0.016);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    EXPECT_EQ(received.size(), 1u);

    // A producer publishing that name resolves the input: nothing more to say.
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer");
    producer->setCamera(camera);
    producer->setRenderTarget(RenderTargetPtr(new RenderTarget()));
    producer->setOutputName(u8"GBuffer");
    engine->addPass(producer, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // The producer leaves again: the same problem, so it is reported again.
    engine->removePass(producer.get());
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief Two producers under ONE output name are reported, not silently last-wins.
 *
 * The named-output registry is a flat map keyed by the output name, so a second producer under a
 * name a consumer already resolves simply overwrites the first: the consumer samples a target the
 * caller never wired it to — and each pass on its own is perfectly valid, so nothing else can see
 * it. Reported once per episode, like the unresolved-input case next to it.
 */
TEST(RenderEngineTest, TwoProducersUnderOneOutputNameAreReportedOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    const auto make_producer = [&camera](const vn::String& name, RenderTargetPtr target) {
        auto pass = intrusive_ptr<RenderPass>(new RenderPass());
        pass->setName(name);
        pass->setCamera(camera);
        pass->setRenderTarget(std::move(target));
        pass->setOutputName(u8"shared");
        return pass;
    };
    auto first  = make_producer(u8"first", RenderTargetPtr(new RenderTarget()));
    auto second = make_producer(u8"second", RenderTargetPtr(new RenderTarget()));
    engine->addPass(first, -2);
    engine->addPass(second, -1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"shared"), vn::String::npos);

    // The same collision next frame is the SAME episode: not one message per frame.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // One producer leaves: the name is wired again, so the report re-arms...
    engine->removePass(second.get());
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // ...and the collision coming back is reported again.
    engine->addPass(second, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief An output name the engine cannot publish is reported, not dropped silently.
 *
 * The named-output registry maps a name to a SAMPLEABLE target, so a pass that publishes while it
 * renders into the window (no render target) asks for something the engine cannot do. Dropping it
 * silently leaves the mistake pointing at the wrong place: a consumer of that name finds nothing
 * and is told "nothing produced it this frame", which accuses the consumer of a wiring bug the
 * PRODUCER made. Reported once per episode, like every other wiring problem.
 */
TEST(RenderEngineTest, PublishingANameWithoutARenderTargetIsReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    auto presenter = intrusive_ptr<RenderPass>(new RenderPass());
    presenter->setName(u8"presenter");
    presenter->setCamera(camera);
    presenter->setOutputName(u8"SceneColor");   // renders into the window: nothing to publish
    engine->addPass(presenter, 0);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"presenter"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"SceneColor"), vn::String::npos);

    // The same problem next frame is the same episode: one message, not one per frame.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // Giving the pass a target makes the publication possible, which ends the episode...
    presenter->setRenderTarget(RenderTargetPtr(new RenderTarget()));
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    // ...so taking it away again is a NEW episode, reported again.
    presenter->setRenderTarget(nullptr);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief A declaration change lands in the frame that follows it, with nothing to notify.
 *
 * What this replaces: the checks used to run only when a declaration "moved" (every pass had a wiring
 * revision for the engine to compare, and the host's bindings had a flag), because running them per frame
 * meant building a map of who fills what for every pass and every attachment to answer a question whose answer
 * had not changed. The frame is COMPILED from the declarations now, so the same work answers a question about
 * THIS frame - and there is no revision to bump, no gate to forget and no host-side flag to keep in step. What
 * this pins is the pair of facts that replaced the gate: a declaration is read by the next frame, and a wrong
 * one is still an EPISODE (once while it lasts, again when it comes back).
 */
TEST(RenderEngineTest, ADeclarationChangeLandsInTheFrameThatFollowsIt)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    auto main_pass = intrusive_ptr<RenderPass>(new RenderPass());
    main_pass->setCamera(camera);
    engine->addPass(main_pass, 0);

    // A steady frame stream says nothing, and stays that way when a declaration moves LEGALLY: the frame
    // simply uses the new declaration.
    engine->frame(0.016);
    engine->frame(0.016);
    const std::size_t quiet = engine->diagnosticCount();
    auto              first_target = RenderTargetPtr(new RenderTarget());
    main_pass->setRenderTarget(first_target);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), quiet) << "a pass drawing into a target is not a problem to report";

    // A declaration that becomes WRONG in place is reported by the frame that sees it - no setter had to tell
    // the engine it moved - and it stays ONE episode while it lasts.
    main_pass->setOutputName(u8"SceneColor");
    engine->frame(0.016);
    const std::size_t problems_before = engine->diagnosticCount();
    main_pass->setRenderTarget(nullptr);   // publishes an output name while rendering into the window
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), problems_before + 1u)
        << "promising an output while rendering into the window is reported by the next frame";
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), problems_before + 1u) << "still one episode, not one per frame";

    // Fixed, then broken again: that is a NEW episode (the compile forgot the problem when it went away).
    main_pass->setRenderTarget(first_target);
    engine->frame(0.016);
    main_pass->setRenderTarget(nullptr);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), problems_before + 2u) << "a wire that breaks again is reported again";

    // The host's bindings are declarations the compile reads as well, and publish()/unpublish() have no pass to
    // state a revision through: what a frame resolves through a name is decided by the compile, so withdrawing
    // a binding is felt by the next frame with nothing to mark.
    auto host_target = RenderTargetPtr(new RenderTarget());
    engine->publish(u8"Host", host_target);
    engine->frame(0.016);
    EXPECT_EQ(engine->resolve(u8"Host"), host_target.get()) << "the compile reads the binding every frame";
    engine->unpublish(u8"Host");
    EXPECT_EQ(engine->resolve(u8"Host"), nullptr) << "and stops answering it as soon as the host withdraws it";
}

/**
 * @brief A pass declared as a VALUE: no subclass, no setter chain, and the plan says what it was.
 *
 * The point of the descriptor entry (M7's "adding a pass is adding a descriptor"): a caller that wants a pass
 * writes one, and the engine decides how to execute it. What the plan reports is the declaration as the frame
 * saw it - including the STAGE it asked for, which is the order it runs at, and the TAG, which is the label a
 * human reads (a name says which pass it is, a tag says what it is for).
 */
TEST(RenderEngineTest, APassCanBeDeclaredAsAValueAndRunsLikeAnyOther)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto scene  = intrusive_ptr<Scene>(new Scene());
    auto target = RenderTargetPtr(new RenderTarget());

    int enabled_calls  = 0;
    int disabled_calls = 0;
    PassDesc desc;
    desc.name          = u8"declared_pass";
    desc.tag           = u8"Lighting";
    desc.stage         = PipelineStage::Shading;
    desc.kind          = PassKind::Scene;
    desc.camera        = camera;
    desc.content       = scene;
    desc.writes        = target;
    desc.output_name   = u8"SceneColor";
    desc.clear_enabled = true;
    desc.before_draw   = [&enabled_calls] { ++enabled_calls; };
    engine->addPass(desc);

    engine->frame(0.016);
    EXPECT_EQ(enabled_calls, 1) << "the hook the declaration carried runs before the pass does";

    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 1u);
    EXPECT_EQ(plan.passes[0].name, vn::String(u8"declared_pass"));
    EXPECT_EQ(plan.passes[0].tag, vn::String(u8"Lighting")) << "the tag is what the plan carries for a human";
    EXPECT_EQ(plan.passes[0].order, pipelineStageOrder(PipelineStage::Shading))
        << "the stage is the order: nobody picks an order number for a declared pass";
    EXPECT_EQ(plan.passes[0].target, target.get());
    EXPECT_TRUE(plan.passes[0].clears);
    EXPECT_EQ(engine->resolve(u8"SceneColor"), target.get()) << "and it published the name it declared";

    // A DISABLED declaration keeps its row and costs nothing - the hook included.
    PassDesc disabled;
    disabled.name        = u8"declared_pass_disabled";
    disabled.kind        = PassKind::Scene;
    disabled.camera      = camera;
    disabled.content     = scene;
    disabled.enabled     = false;
    disabled.before_draw = [&disabled_calls] { ++disabled_calls; };
    engine->addPass(disabled);
    engine->frame(0.016);
    EXPECT_EQ(disabled_calls, 0) << "a pass that does not run does not update its content either";
    EXPECT_EQ(enabled_calls, 2) << "while the one that runs keeps updating its own";
    const FramePlan with_disabled = engine->framePlan();
    ASSERT_EQ(with_disabled.passes.size(), 2u);
    EXPECT_FALSE(with_disabled.passes[1].enabled);
}

/**
 * @brief The same value entry declares a SCREEN pass: the kind picks the execution, the declaration the picture.
 *
 * Before M7 this was the one pass kind a host had to have a class for (ScreenPass), and its program/inputs had
 * to be wired by hand. Now the kind says how it executes and everything else is the declaration - so a
 * compositing step is a value like any other, and the mock backend can be asked whether the draw happened with
 * the source the compile RESOLVED (not the source the caller guessed).
 */
TEST(RenderEngineTest, AScreenPassCanBeDeclaredAsAValueToo)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto scene  = intrusive_ptr<Scene>(new Scene());
    auto source = RenderTargetPtr(new RenderTarget());

    // The producer: an ordinary scene pass that publishes what the screen pass samples.
    PassDesc producer;
    producer.name        = u8"scene";
    producer.kind        = PassKind::Scene;
    producer.camera      = camera;
    producer.content     = scene;
    producer.writes      = source;
    producer.output_name = u8"SceneColor";
    engine->addPass(producer);

    // The consumer: a screen pass that samples the published name through a program.
    PassDesc compose;
    compose.name        = u8"compose";
    compose.tag         = u8"Compositing";
    compose.stage       = PipelineStage::Transparent;
    compose.kind        = PassKind::Screen;
    compose.camera      = camera;
    compose.program     = screenCopyProgram();
    compose.input_names = { vn::String(u8"SceneColor") };
    engine->addPass(compose);

    engine->frame(0.016);
    EXPECT_EQ(backend->program_draws, 1u) << "the declared screen pass drew through its program";
    EXPECT_EQ(backend->last_program_source, source.get()) << "with the target the compile resolved for it";
    EXPECT_EQ(backend->last_program_camera, camera.get());

    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 2u);
    EXPECT_EQ(plan.passes[1].name, vn::String(u8"compose"));
    EXPECT_EQ(plan.passes[1].inputs.size(), 1u);
    EXPECT_EQ(plan.passes[1].inputs[0], vn::String(u8"SceneColor"))
        << "the plan lists what the pass DECLARED (the resolved address is the frame's business)";
}

/**
 * @brief A declaration states a clear policy only when it HAS one; otherwise the pass' kind decides.
 *
 * The descriptor is not a second set of defaults to keep in step with the node's: silence means "the kind's
 * policy", which is the only reading that keeps a declared pass and a hand-built one the same pass. It matters
 * most where the kinds disagree - a Screen pass composites over what is already there, so a `clear_enabled`
 * default of true would repaint the target it was about to draw over.
 */
TEST(RenderEngineTest, ADeclaredPassClearsTheWayItsKindDoesUntilItSaysOtherwise)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto camera = intrusive_ptr<Camera>(new Camera());

    // Silently declared screen pass: it keeps the kind's "never clears" and the kind's depth policy.
    PassDesc copy;
    copy.name    = u8"copy";
    copy.kind    = PassKind::Screen;
    copy.camera  = camera;
    copy.program = screenCopyProgram();
    const auto copy_node = engine->addPass(copy);
    ASSERT_NE(copy_node, nullptr);
    EXPECT_FALSE(copy_node->isClearEnabled()) << "a silent declaration must not turn a screen pass' clear on";
    EXPECT_FALSE(copy_node->shouldClearDepth());

    // Silently declared scene pass: the other way round, and it clears to the colour a hand-built pass does.
    PassDesc opaque;
    opaque.name   = u8"opaque";
    opaque.camera = camera;
    const auto opaque_node = engine->addPass(opaque);
    ASSERT_NE(opaque_node, nullptr);
    EXPECT_TRUE(opaque_node->isClearEnabled());
    EXPECT_EQ(opaque_node->clearColor(), Color(51, 51, 51, 255));

    // A declaration that states the policy wins over the kind, on each of the three axes on its own.
    PassDesc tinted;
    tinted.name          = u8"tinted";
    tinted.camera        = camera;
    tinted.clear_enabled = true;
    tinted.clear_color   = Color(10, 20, 30, 255);
    tinted.clear_depth   = false;
    const auto tinted_node = engine->addPass(tinted);
    ASSERT_NE(tinted_node, nullptr);
    EXPECT_TRUE(tinted_node->isClearEnabled());
    EXPECT_EQ(tinted_node->clearColor(), Color(10, 20, 30, 255));
    EXPECT_FALSE(tinted_node->shouldClearDepth());
}

/**
 * @brief A transient target is declared, allocated, shared, replaced and let go of - by the compile.
 *
 * The pass says which IMAGE it needs and what shape it must have; the frame owns the target. What this pins is
 * the whole lifetime, which is what "no host has to own the G-buffer anymore" rests on: one target per declared
 * identity (a second declaration of the same identity and shape SHARES it), the SAME target across a steady
 * frame stream (nothing is reallocated per frame), a REPLACEMENT when the declared shape changes, and nothing
 * left behind when the declaration goes away.
 */
TEST(RenderEngineTest, ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));
    area->resize(320, 180);

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto scene  = intrusive_ptr<Scene>(new Scene());

    const auto declare = [&](const vn::String& name, int width, int height, bool depth) {
        PassDesc desc;
        desc.name         = name;
        desc.kind         = PassKind::Scene;
        desc.camera       = camera;
        desc.content      = scene;
        desc.writes_image = u8"gbuffer";
        desc.writes_shape.width  = width;
        desc.writes_shape.height = height;
        desc.writes_shape.colors = { RenderTarget::ColorFormat::RGBA8 };
        desc.writes_shape.depth  = depth ? std::optional<RenderTarget::DepthFormat>(RenderTarget::DepthFormat::D24)
                                         : std::nullopt;
        engine->addPass(desc);
    };

    // Declared with no explicit extent: the shape follows the surface the frame is presenting.
    declare(u8"gbuffer_pass", 0, 0, true);
    EXPECT_EQ(engine->transientTarget(u8"gbuffer"), nullptr)
        << "before the first compiled frame there is no transient: it is the compile's product, not the build's";
    engine->frame(0.016);
    ASSERT_EQ(engine->transientTargetCount(), 1u) << "one declaration, one target";
    const RenderTarget* const first = engine->framePlan().passes[0].target;
    ASSERT_NE(first, nullptr) << "the declare-then-draw hand-off gives the pass the target it asked for";
    EXPECT_EQ(engine->transientTarget(u8"gbuffer"), first)
        << "the owner reaches the instance through the identity it declared";
    EXPECT_EQ(engine->transientTarget(u8"nothing_declares_this"), nullptr)
        << "an identity nobody declared is not a target, it is nothing";
    EXPECT_EQ(first->width(), 320);
    EXPECT_EQ(first->height(), 180);
    EXPECT_TRUE(first->hasDepth()) << "the declaration asked for a depth attachment";

    for (int frame = 0; frame < 4; ++frame) {
        engine->frame(0.016);
        ASSERT_EQ(engine->transientTargetCount(), 1u);
        EXPECT_EQ(engine->framePlan().passes[0].target, first) << "a steady frame stream reallocates nothing";
    }

    // A second pass declaring the SAME image and shape shares the target: one image, one attachment.
    declare(u8"gbuffer_second", 0, 0, true);
    engine->frame(0.016);
    EXPECT_EQ(engine->transientTargetCount(), 1u);
    ASSERT_EQ(engine->framePlan().passes.size(), 2u);
    EXPECT_EQ(engine->framePlan().passes[1].target, first) << "the same declaration is the same target";
    engine->removePass(const_cast<RenderPass*>(engine->framePlan().passes[1].pass));
    engine->frame(0.016);
    EXPECT_EQ(engine->transientTargetCount(), 1u) << "the first declaration still needs it";

    // A DIFFERENT shape is a different target: the attachments of a built one cannot change.
    engine->clearPasses();
    engine->frame(0.016);
    EXPECT_EQ(engine->transientTargetCount(), 0u) << "nothing declares it any more, so the compile let it go";
    EXPECT_EQ(engine->transientTarget(u8"gbuffer"), nullptr) << "and the query answers with what is live now";

    declare(u8"gbuffer_pass", 640, 360, false);
    engine->frame(0.016);
    ASSERT_EQ(engine->transientTargetCount(), 1u);
    const RenderTarget* const second = engine->framePlan().passes[0].target;
    EXPECT_NE(second, nullptr);
    EXPECT_EQ(second->width(), 640) << "the declared extent is the built extent";
    EXPECT_EQ(second->height(), 360);
    EXPECT_FALSE(second->hasDepth()) << "and the declared attachment set is the built one";
    EXPECT_EQ(engine->transientTarget(u8"gbuffer"), second)
        << "a new shape is a NEW instance, and the query answers with the live one";
    // The released instance is NOT witnessed by its ADDRESS: a freed target's memory may be handed straight back
    // to the next one (measured 2026-10-11: the same pointer came back once in a few runs, which made this line a
    // test of the allocator). What the engine promises is the live instance's extent and that only ONE exists -
    // which is exactly why a handle kept across a resize describes a target the compile has let go.
    EXPECT_EQ(engine->transientTargetCount(), 1u);
    EXPECT_EQ(engine->transientTarget(u8"gbuffer")->width(), 640);
}

/**
 * @brief A host that publishes a name without a target is told, not ignored.
 *
 * The host-facing half of the same rule: publish() exists so an application can hand a target to
 * by-name consumers, and a null target means there is nothing to hand out. Ignoring it silently
 * leaves the host believing the name is served.
 */
TEST(RenderEngineTest, PublishingANullTargetIsReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    engine->publish(u8"External", RenderTargetPtr(nullptr));

    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"External"), vn::String::npos);
    EXPECT_EQ(engine->resolve(u8"External"), nullptr);

    // The same call again is the same episode; a real target ends it, so the next failed publish
    // is a new episode.
    engine->publish(u8"External", RenderTargetPtr(nullptr));
    EXPECT_EQ(received.size(), 1u);
    engine->publish(u8"External", RenderTargetPtr(new RenderTarget()));
    engine->publish(u8"External", RenderTargetPtr(nullptr));
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief One wire, one message: the name and the object declarations report different mistakes.
 *
 * A pass may declare its output by name, as an image, as a whole target — or several of them. Two
 * such passes can go wrong in two independent ways, and each is reported by exactly one check:
 *   * they claim the SAME target (both write it, both promise it) -> the wiring check reports the
 *     claim, and the name map sees one name for ONE target, which is not a collision at all;
 *   * they publish the SAME name for DIFFERENT targets -> the name map reports it, and the wiring
 *     check sees two targets claimed by one pass each, which is legal wiring.
 * Pinned here so the three declarations cannot start reporting one mistake twice.
 */
TEST(RenderEngineTest, OneWireIsReportedByOneCheckWhetherDeclaredByNameOrObject)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    const auto make_pass = [&camera](const vn::String& name, RenderTargetPtr target, RenderTargetPtr promise) {
        auto pass = intrusive_ptr<RenderPass>(new RenderPass());
        pass->setName(name);
        pass->setCamera(camera);
        pass->setRenderTarget(std::move(target));
        pass->setOutputName(u8"shared");
        if (promise != nullptr) {
            pass->setOutputTarget(std::move(promise));
        }
        return pass;
    };

    // Two producers writing ONE target and publishing one name: the claim is the mistake (the name
    // map sees a single target under the name), so the object check reports it — once. The target
    // needs an attachment: a promise is validated per IMAGE, and a target with none has no image to
    // claim.
    auto shared_target = RenderTargetPtr(new RenderTarget());
    shared_target->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto first         = make_pass(u8"first", shared_target, shared_target);
    auto second        = make_pass(u8"second", shared_target, shared_target);
    engine->addPass(first, -2);
    engine->addPass(second, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_NE(received[0].message.find(u8"claim"), vn::String::npos);
    engine->removePass(first.get());
    engine->removePass(second.get());

    // Two producers writing DIFFERENT targets under one name: the name map reports it — once — and
    // the claims are legal wiring (one pass each).
    received.clear();
    auto third_target = RenderTargetPtr(new RenderTarget());
    third_target->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto fourth_target = RenderTargetPtr(new RenderTarget());
    fourth_target->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto third  = make_pass(u8"third", third_target, nullptr);
    auto fourth = make_pass(u8"fourth", fourth_target, nullptr);
    engine->addPass(third, -2);
    engine->addPass(fourth, -1);
    engine->frame(0.016);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_NE(received[0].message.find(u8"shared"), vn::String::npos);
}

/**
 * @brief Publishing the SAME target under one name from two passes is not a collision.
 *
 * AppShellUi registers two passes that publish one shared target under one name; a consumer
 * resolves the same image either way, so reporting that would be noise.
 */
TEST(ImageRefTest, LabelKindAndBindingRoundTrip)
{
    // An ImageRef identifies one image: a label for diagnostics, a kind, and the image it currently
    // is. Unbound is the initial state (a producer that has not run yet), not an error.
    auto color = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.color"));
    EXPECT_EQ(color->label(), u8"GBuffer.color");
    EXPECT_EQ(color->kind(), ImageRef::Kind::Color);
    EXPECT_FALSE(color->isBound());
    EXPECT_EQ(color->target(), nullptr);
    EXPECT_EQ(color->attachment(), 0);

    auto depth = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.depth", ImageRef::Kind::Depth));
    EXPECT_EQ(depth->kind(), ImageRef::Kind::Depth);

    auto target = RenderTargetPtr(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(320, 180);
    color->bind(target, 2);
    EXPECT_TRUE(color->isBound());
    EXPECT_EQ(color->target(), target.get());
    EXPECT_EQ(color->attachment(), 2);

    // Unbinding is how "the image is gone" is expressed, so a consumer can
    // never be handed a stale one.
    color->unbind();
    EXPECT_FALSE(color->isBound());
    EXPECT_EQ(color->target(), nullptr);
    EXPECT_EQ(color->attachment(), 0);
}

/**
 * @brief A bound image keeps its target alive: the hand-off describes itself.
 *
 * The host owns the target, but an image that merely referenced it would leave a consumer holding a
 * dangling image the moment its owner dropped the target, and the wiring mistake would only surface
 * later as a crash or a blank frame. The strong reference here is what makes "image->target()" safe
 * to read at any time.
 */
TEST(ImageRefTest, HoldsItsTargetAlive)
{
    auto image = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.color"));
    {
        auto target = RenderTargetPtr(new RenderTarget());
        target->setSize(64, 32);
        image->bind(target, 1);
    }
    // The owner is gone; the image is still bound.
    ASSERT_TRUE(image->isBound());
    ASSERT_NE(image->target(), nullptr);
    EXPECT_EQ(image->target()->width(), 64);
    EXPECT_EQ(image->target()->height(), 32);
    EXPECT_EQ(image->attachment(), 1);
}

TEST(RenderPassTest, ObjectTypedOutputAndInputImages)
{
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    EXPECT_EQ(pass->output(), nullptr);
    EXPECT_TRUE(pass->inputs().empty());

    auto color = intrusive_ptr<ImageRef>(new ImageRef(u8"SceneColor"));
    auto depth = intrusive_ptr<ImageRef>(new ImageRef(u8"SceneDepth", ImageRef::Kind::Depth));

    pass->setOutput(color);
    EXPECT_EQ(pass->output(), color.get());

    // Inputs keep declaration order: ScreenPass samples the first one that
    // resolved, so the order a caller writes them in is the priority.
    pass->addInput(color);
    pass->addInput(depth);
    ASSERT_EQ(pass->inputs().size(), 2u);
    EXPECT_EQ(pass->inputs()[0].get(), color.get());
    EXPECT_EQ(pass->inputs()[1].get(), depth.get());

    // The object-typed API is a separate axis from the name API: neither
    // clears the other (a pass mid-migration can declare both).
    pass->setOutputName(u8"SceneColor");
    EXPECT_EQ(pass->output(), color.get());
    EXPECT_EQ(pass->outputName(), u8"SceneColor");

    pass->clearInputs();
    EXPECT_TRUE(pass->inputs().empty());
    EXPECT_TRUE(pass->output() != nullptr);

    pass->setOutput(intrusive_ptr<ImageRef>());
    EXPECT_EQ(pass->output(), nullptr);
}

/**
 * @brief One image declared as the output of TWO passes is reported, once per episode.
 *
 * This is the object-typed twin of the two-producers-under-one-name case: both passes write the
 * SAME addressable image, so whichever runs last silently overwrites what the other promised and a
 * consumer got an image nobody wired it to. Each pass on its own is valid, so nothing else can see
 * it — and because the image is where the hand-off lives, it is visible from the declarations alone
 * (design §14.4), not only at run time.
 */
TEST(RenderEngineTest, ImageDeclaredAsOutputByTwoPassesIsReportedOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto image  = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.color"));

    // No output NAMES are declared, so the name registry stays silent: the single message can only
    // come from the image declarations (the two rules must not double-report one mistake).
    const auto make_producer = [&camera, &image](const vn::String& name, RenderTargetPtr target) {
        auto pass = intrusive_ptr<RenderPass>(new RenderPass());
        pass->setName(name);
        pass->setCamera(camera);
        pass->setRenderTarget(std::move(target));
        pass->setOutput(image);
        return pass;
    };
    auto first  = make_producer(u8"first", RenderTargetPtr(new RenderTarget()));
    auto second = make_producer(u8"second", RenderTargetPtr(new RenderTarget()));
    engine->addPass(first, -2);
    engine->addPass(second, -1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"GBuffer.color"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"first"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"second"), vn::String::npos);

    // The same collision next frame is the SAME episode: not one message per frame.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // One producer leaves: the collision is over, and what is left is the promise of an UNBOUND image - which
    // the same compile sees and now says out loud. (The old episode bookkeeping could not: it marked the image
    // as "already accounted for" in the frame that reported the COLLISION, so the declaration stayed silent
    // afterwards even though nothing had ever been said about it. In the compile, an episode is what was
    // actually reported, so a problem that was never spoken about is not treated as one that was.)
    engine->removePass(second.get());
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    ASSERT_EQ(received.size(), 2u);
    EXPECT_NE(received[1].message.find(u8"not bound to a target"), vn::String::npos);
    EXPECT_NE(received[1].message.find(u8"first"), vn::String::npos);

    // The collision coming back is reported again (the episode re-arms), and it takes the place of the
    // unbound-promise message: one image, one message, whichever of the two rules speaks for it.
    engine->addPass(second, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 3u);
    EXPECT_EQ(received.size(), 3u);
    EXPECT_NE(received[2].message.find(u8"declared as the output of two passes"), vn::String::npos);

    // Two passes declaring the SAME object are one role, not a collision: only a second, DIFFERENT
    // pass on the image is. The same pass cannot collide with itself.
    engine->removePass(first.get());
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 4u) << "the remaining promise is about an unbound image, and says so";
    ASSERT_EQ(received.size(), 4u);
    EXPECT_NE(received[3].message.find(u8"not bound to a target"), vn::String::npos);
    EXPECT_NE(received[3].message.find(u8"second"), vn::String::npos);
}

/**
 * @brief Two DIFFERENT ImageRefs that turn out to be one image are a collision too.
 *
 * The identity of an image is its ADDRESS (target + attachment), not the object the host happened
 * to declare it with: a pipeline assembled from two references to the same attachment has two
 * producers for one image, and whoever runs last silently wins — exactly the case the name-based
 * check used to catch and the object-based one could not. The message names both declarations, so a
 * reader can see WHY they are the same image.
 */
TEST(RenderEngineTest, TwoImageRefsOfOneAttachmentAreReportedOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto target = RenderTargetPtr(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);

    // Two independent declarations of the SAME attachment (nothing shares the object).
    auto declared_by_first  = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.albedo"));
    auto declared_by_second = intrusive_ptr<ImageRef>(new ImageRef(u8"albedo_copy"));
    declared_by_first->bind(target, 0);
    declared_by_second->bind(target, 0);

    const auto make_producer = [&camera, &target](const vn::String& name, intrusive_ptr<ImageRef> image) {
        auto pass = intrusive_ptr<RenderPass>(new RenderPass());
        pass->setName(name);
        pass->setCamera(camera);
        pass->setRenderTarget(target);
        pass->setOutput(std::move(image));
        return pass;
    };
    auto first  = make_producer(u8"first", declared_by_first);
    auto second = make_producer(u8"second", declared_by_second);
    engine->addPass(first, -2);
    engine->addPass(second, -1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    // Both declarations are named: the point of the report is that they are one image.
    EXPECT_NE(received[0].message.find(u8"GBuffer.albedo"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"albedo_copy"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"first"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"second"), vn::String::npos);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // One producer leaves: the image has a single writer again, so the episode re-arms...
    engine->removePass(second.get());
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // ...and the collision coming back is reported again.
    engine->addPass(second, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief Two images of ONE target with different attachments are not a collision.
 *
 * A pass binds ALL of its target's attachments, so several passes may write one target with
 * different declared images — an MRT pass hands out several images at once, and the deferred
 * pipeline renders two passes into `composite` (light and transparent). Keying the check on the
 * target alone would report that legitimate pipeline.
 */
TEST(RenderEngineTest, TwoImagesOfOneTargetWithDifferentAttachmentsAreNotACollision)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto target = RenderTargetPtr(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachColor(RenderTarget::ColorFormat::RGBA16F);

    auto albedo = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.albedo"));
    auto normal = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.normal"));
    albedo->bind(target, 0);
    normal->bind(target, 1);

    const auto make_pass = [&camera, &target](const vn::String& name, intrusive_ptr<ImageRef> image) {
        auto pass = intrusive_ptr<RenderPass>(new RenderPass());
        pass->setName(name);
        pass->setCamera(camera);
        pass->setRenderTarget(target);
        pass->setOutput(std::move(image));
        return pass;
    };
    engine->addPass(make_pass(u8"gbuffer", albedo), -2);
    engine->addPass(make_pass(u8"light", normal), -1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(received.empty());
}

/**
 * @brief Two DIFFERENT unbound images declared as outputs are not one collision.
 *
 * An unbound image has no address, so the declared object is the only identity that exists — and two
 * distinct objects are two distinct images as far as anyone can tell: nothing claims one image
 * twice. Each of them IS reported for being unbound (an unbound declaration cannot be delivered, and
 * the promise cannot be checked — see AnUnboundDeclaredImageIsReportedAtWiringTime); what is pinned
 * here is that the two are not treated as ONE image, which a collapsed identity would make them.
 * The same object declared twice IS a collision (see
 * ImageDeclaredAsOutputByTwoPassesIsReportedOnce).
 */
TEST(RenderEngineTest, TwoDistinctUnboundImagesAreNotACollision)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    const auto make_pass = [&camera](const vn::String& name, intrusive_ptr<ImageRef> image) {
        auto pass = intrusive_ptr<RenderPass>(new RenderPass());
        pass->setName(name);
        pass->setCamera(camera);
        pass->setRenderTarget(RenderTargetPtr(new RenderTarget()));
        pass->setOutput(std::move(image));
        return pass;
    };
    engine->addPass(make_pass(u8"first", intrusive_ptr<ImageRef>(new ImageRef(u8"not_bound_yet"))), -2);
    engine->addPass(make_pass(u8"second", intrusive_ptr<ImageRef>(new ImageRef(u8"also_not_bound"))), -1);

    engine->frame(0.016);

    // One report per unbound image, neither of them about claiming an image: a collapsed identity
    // ("any unbound image") would make the two collide and say so instead.
    ASSERT_EQ(received.size(), 2u);
    for (const auto& diagnostic : received) {
        EXPECT_EQ(diagnostic.category, DiagnosticCategory::ContentSkipped);
        EXPECT_NE(diagnostic.message.as_std_str().find("not bound to a target"), std::string::npos);
        EXPECT_EQ(diagnostic.message.as_std_str().find("claim"), std::string::npos);
    }
    // The two messages are about their own image (each names its own label).
    EXPECT_NE(received[0].message.as_std_str(), received[1].message.as_std_str());
}

/**
 * @brief A declared input image NOBODY produces is reported, once per episode.
 *
 * The wiring is the engine's job: it can see that no pass declares the image, so the consumer can
 * never be filled — and each pass on its own is perfectly valid, so nothing else can. This is the
 * structural half of D37 brought forward to wiring time (design §14.4): the name-based counterpart
 * of the same fact can only be judged once a frame has run, because a name resolves per frame.
 */
TEST(RenderEngineTest, DeclaredInputImageWithoutProducerIsReportedOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"gbuffer");
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto image = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.albedo"));
    image->bind(source, 0);

    auto consumer = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"light");
    consumer->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    consumer->setProgram(vn::graphics::screenCopyProgram());
    consumer->addInput(image);
    engine->addPass(consumer, 0);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"light"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"GBuffer.albedo"), vn::String::npos);

    // Still nobody: reported once, not once per frame.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // A producer appears BEFORE the consumer, drawing into the very target it promises: the wire is
    // whole, so nothing more to say.
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer");
    producer->setCamera(camera);
    producer->setRenderTarget(source);
    producer->setOutput(image);
    engine->addPass(producer, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // The producer leaves again: the same problem, so it is reported again.
    engine->removePass(producer.get());
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief A producer registered AFTER its consumer is reported: the image is a frame behind.
 *
 * The pass will draw from an image that is filled later in the same frame, so it samples what was
 * there last time (or nothing). Only the engine can see the registration order of a declaration
 * pair, and the fix (register the producer first, or lower its order) is not something a reader of
 * either pass can infer.
 */
TEST(RenderEngineTest, InputImageProducedByALaterPassIsReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"gbuffer");
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto image = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.albedo"));
    image->bind(source, 0);

    auto consumer           = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"light");
    consumer->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    consumer->setProgram(vn::graphics::screenCopyProgram());
    consumer->addInput(image);
    engine->addPass(consumer, 0);

    // Registered AFTER the consumer (higher order), drawing into the target it promises.
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer");
    producer->setCamera(camera);
    producer->setRenderTarget(source);
    producer->setOutput(image);
    engine->addPass(producer, 5);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"light"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"gbuffer"), vn::String::npos);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // The producer moves before the consumer: fixed, and nothing more to say.
    engine->removePass(producer.get());
    engine->addPass(producer, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // It moves back: the same problem, reported again.
    engine->removePass(producer.get());
    engine->addPass(producer, 5);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief A pass that reads the target it draws into is left to the backend, not reported twice.
 *
 * Reading what you are writing is the feedback pattern: whether the actual draw call is a feedback
 * loop depends on the layouts the backend manages, and it rejects source == destination with its own
 * report. The engine's two layers (wiring and frame) stay quiet about it, so one mistake is stated
 * once — by the layer that can judge it.
 */
TEST(RenderEngineTest, PassReadingTheTargetItDrawsIntoIsLeftToTheBackend)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"frame");
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto image = intrusive_ptr<ImageRef>(new ImageRef(u8"frame"));
    image->bind(target, 0);

    auto pass = intrusive_ptr<ScreenPass>(new ScreenPass());
    pass->setName(u8"overlay");
    pass->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    pass->setProgram(vn::graphics::screenCopyProgram());
    pass->setRenderTarget(target);   // it draws into the very target it reads
    pass->setOutput(image);
    pass->addInput(image);
    engine->addPass(pass, 0);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(received.empty());
}

/**
 * @brief A surface size announced BEFORE initialize reaches the backend once it can honour it.
 *
 * A host knows its surface size before it has a backend to tell (it sizes its widget first), and the
 * engine used to drop that announcement: the backend was only ever told about resizes that happened
 * after initialization, so the first frames ran at the backend's default size. The AREA now owns the
 * announcement (RenderSurface::resize is the one spelling - see RenderEngine, which keeps no size of
 * its own): it records the numbers in the frame context (which the passes lay themselves out on) and
 * hands them over as soon as initialization succeeded.
 */
TEST(RenderEngineTest, TheAreasSizeAnnouncedBeforeInitializeReachesTheBackend)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));

    // Announced first: the backend does not exist yet as far as the size is concerned.
    area->resize(800, 600);
    EXPECT_EQ(backend->surface_half.resize_calls, 0);
    EXPECT_EQ(engine->frameContext().surface_width, 800);
    EXPECT_EQ(engine->frameContext().surface_height, 600);

    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(backend->surface_half.resize_calls, 1);
    EXPECT_EQ(backend->surface_half.last_surface_width, 800);
    EXPECT_EQ(backend->surface_half.last_surface_height, 600);

    // A later resize is forwarded directly (one call per announcement, not two).
    area->resize(1024, 768);
    EXPECT_EQ(backend->surface_half.resize_calls, 2);
    EXPECT_EQ(backend->surface_half.last_surface_width, 1024);
    EXPECT_EQ(backend->surface_half.last_surface_height, 768);
}

/**
 * @brief A size announced before initialize is not invented when nobody ever announced one.
 *
 * The frame starts at 0x0 (no announcement), and handing a 0-sized surface to the backend would be
 * worse than saying nothing: the backend keeps whatever default its surface has.
 */
TEST(RenderEngineTest, NoSurfaceSizeAnnouncementIsNotForwarded)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(*engine));

    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(backend->surface_half.resize_calls, 0);

    // A non-positive announcement is ignored (the engine does not record it either).
    area->resize(0, 0);
    EXPECT_EQ(backend->surface_half.resize_calls, 0);
    EXPECT_EQ(engine->frameContext().surface_width, 0);
}

/**
 * @brief A whole-target promise answers a fine (one attachment) read of that target.
 *
 * The two declaration layers meet at the identity: an MRT producer promises the target (one line,
 * shape-agnostic — a pass filling a target writes ALL its attachments in one render scope), and a PiP
 * that samples attachment 3 of it declares exactly that one image. Neither side has to know how many
 * attachments the other wrote.
 */
TEST(RenderEngineTest, WholeTargetPromiseSatisfiesAFineImageRead)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto gbuffer = RenderTargetPtr(new RenderTarget());
    gbuffer->setName(u8"gbuffer");
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA8);
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA16F);
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA16F);

    // Producer: fills the target and promises the whole of it.
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer_pass");
    producer->setCamera(camera);
    producer->setRenderTarget(gbuffer);
    producer->setOutputTarget(gbuffer);
    engine->addPass(producer, -3);

    // Consumer: the one image it samples (attachment 2), declared at its own grain.
    auto sampled = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.position"));
    sampled->bind(gbuffer, 2);
    auto consumer = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"preview");
    consumer->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    consumer->setProgram(vn::graphics::screenCopyProgram());
    consumer->addInput(sampled);
    engine->addPass(consumer, 10);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(received.empty());
}

/**
 * @brief Reading a whole target nobody writes is reported: the unit really consumed is the target.
 *
 * A fullscreen program receives every colour attachment of its source, so it declares the target —
 * and if no pass renders into it, the program has nothing to read. One declaration, one report,
 * however many attachments the target has.
 */
TEST(RenderEngineTest, WholeTargetReadWithoutAnyWriterIsReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto orphan = RenderTargetPtr(new RenderTarget());
    orphan->setName(u8"never_written");
    orphan->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto light = intrusive_ptr<ScreenPass>(new ScreenPass());
    light->setName(u8"deferred_lighting");
    light->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    light->setProgram(vn::graphics::screenCopyProgram());
    light->addInputTarget(orphan);
    engine->addPass(light, 0);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"deferred_lighting"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"never_written"), vn::String::npos);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // A writer appears (it fills the target; no promise needed): the read is answered.
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"fill");
    producer->setCamera(camera);
    producer->setRenderTarget(orphan);
    engine->addPass(producer, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // The writer leaves: reported again.
    engine->removePass(producer.get());
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
}

/**
 * @brief The depth of a target is a DIFFERENT image from its colour attachment 0.
 *
 * The fullscreen-program path samples a depth buffer to reconstruct positions, and a depth read has
 * to be answered by a target that actually has one: a program asking for the depth of a colour-only
 * target reads nothing, which is a wiring mistake the fine (ImageRef) form can now state — the
 * coarse form cannot, because "the target" does not say which of its images is wanted.
 */
TEST(RenderEngineTest, DepthReadIsReportedWhenTheTargetHasNoDepth)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto color_only = RenderTargetPtr(new RenderTarget());
    color_only->setName(u8"composite");
    color_only->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"light");
    producer->setCamera(camera);
    producer->setRenderTarget(color_only);   // fills the target, but it has no depth
    engine->addPass(producer, -1);

    auto depth_image = intrusive_ptr<ImageRef>(new ImageRef(u8"Composite.depth", ImageRef::Kind::Depth));
    depth_image->bind(color_only, 0);
    auto consumer = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"reconstruct");
    consumer->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    consumer->setProgram(vn::graphics::screenCopyProgram());
    consumer->addInput(depth_image);
    engine->addPass(consumer, 1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"Composite.depth"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"composite"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"depth"), vn::String::npos);
}

/**
 * @brief A declared image nobody bound to a target is reported at wiring time.
 *
 * An unbound ImageRef has no address, so nothing can be delivered through it: resolveDeclaredImage()
 * returns null and says nothing (its comment says the wiring check reports the declaration — this test
 * is what makes that true). It also silently disables the promise check, which compares the promised
 * target with the one the pass renders into and has nothing to compare while the image is unbound.
 * One report per IMAGE, because a producer and its consumers share the same unbound object.
 */
TEST(RenderEngineTest, AnUnboundDeclaredImageIsReportedAtWiringTime)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    auto target = RenderTargetPtr(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto unbound = intrusive_ptr<ImageRef>(new ImageRef(u8"SceneColor"));

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"scene");
    producer->setCamera(camera);
    producer->setRenderTarget(target);
    producer->setOutput(unbound);   // declared, but never bound to that target
    engine->addPass(producer, -1);

    auto consumer = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"present");
    consumer->setCamera(camera);
    consumer->setProgram(vn::graphics::screenCopyProgram());
    consumer->addInput(unbound);
    engine->addPass(consumer, 1);

    const int draws_before = backend->program_draws;
    engine->frame(0.016);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"SceneColor"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"bind"), vn::String::npos);
    // What the report describes is what happens: the consumer resolves nothing and draws nothing.
    EXPECT_EQ(backend->program_draws - draws_before, 0);

    // Same problem next frame: one episode, one message (the runtime path stays silent about it).
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // Binding the image wires the wire: the report stops and the consumer samples the target.
    unbound->bind(target, 0);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    EXPECT_EQ(backend->program_draws - draws_before, 1);
    EXPECT_EQ(backend->last_program_source, target.get());
}

/**
 * @brief A ScreenPass with no program is reported and draws nothing — and a depth-only declaration
 *        is no longer a problem at all.
 *
 * A ScreenPass has no implicit shading: the program it names IS its picture, so a pass with none
 * draws nothing, and the engine says so once per episode (the fix is one line).
 *
 * The rule this test replaced belonged to the deleted copy path: a program-less pass could only
 * sample ONE COLOUR attachment, so declaring just the depth was a mistake it had to report. With a
 * program named, what the pass can sample is the source's ABI (binding i = attachment i), so the
 * same declaration is simply fine — which is the half of the rule worth pinning, because it is the
 * half that would go unnoticed if the old check were kept around.
 */
TEST(RenderEngineTest, ScreenPassWithoutAProgramIsReportedAndDrawsNothing)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    // A producer that fills a target with a colour attachment AND depth: the declaration below is
    // fine, the missing program is the only problem.
    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"composite");
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"light");
    producer->setCamera(camera);
    producer->setRenderTarget(target);
    engine->addPass(producer, -1);

    auto depth_image = intrusive_ptr<ImageRef>(new ImageRef(u8"Composite.depth", ImageRef::Kind::Depth));
    depth_image->bind(target, 0);
    auto consumer = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"reconstruct");
    consumer->setCamera(camera);
    consumer->addInput(depth_image);
    engine->addPass(consumer, 1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"reconstruct"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"program"), vn::String::npos);
    // It resolved its source and still drew nothing: "no program" is not "no input".
    EXPECT_EQ(consumer->sourceTarget(), target.get());
    EXPECT_EQ(backend->program_draws, 0);

    // Same problem next frame: one episode, one message.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    EXPECT_EQ(backend->program_draws, 0);

    // Naming a program ends the episode, and the pass draws — depth-only declaration and all, which is
    // what the deleted "a screen pass cannot sample that" rule used to refuse.
    auto program = vn::graphics::screenCopyProgram();
    consumer->setProgram(program);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    EXPECT_EQ(backend->program_draws, 1);
    EXPECT_EQ(backend->last_program_source, target.get());
    EXPECT_EQ(backend->last_program, program.get());
}

/**
 * @brief Colour attachment 0 and the depth of one target are NOT the same image.
 *
 * In a naive "index into the target" model both are attachment 0, so two passes promising one each
 * would look like a collision; they are different images (a program samples one or the other), which
 * is why the identity carries the kind. A promise of the depth and a promise of colour 0 must coexist.
 */
TEST(RenderEngineTest, DepthAndColorAttachmentZeroAreDistinctImages)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"composed");
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);

    auto depth_image = intrusive_ptr<ImageRef>(new ImageRef(u8"composed.depth", ImageRef::Kind::Depth));
    depth_image->bind(target, 0);
    auto color_image = intrusive_ptr<ImageRef>(new ImageRef(u8"composed.color"));
    color_image->bind(target, 0);

    auto depths = intrusive_ptr<RenderPass>(new RenderPass());
    depths->setName(u8"depth_owner");
    depths->setCamera(camera);
    depths->setRenderTarget(target);
    depths->setOutput(depth_image);

    auto colors = intrusive_ptr<RenderPass>(new RenderPass());
    colors->setName(u8"color_owner");
    colors->setCamera(camera);
    colors->setRenderTarget(target);   // a promise has to be about what the pass writes
    colors->setOutput(color_image);
    engine->addPass(depths, 0);
    engine->addPass(colors, 1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(received.empty());
}

/**
 * @brief Two passes promising the SAME image, one of them as a whole target, are reported.
 *
 * A promise is a claim about whose content a consumer gets; two claims on one image mean whoever runs
 * last wins silently. A whole-target promise covers each of the target's images, so it collides with
 * a fine promise of any one of them — the comparison is per image, not per declaration.
 */
TEST(RenderEngineTest, WholeTargetPromiseCollidingWithAFineOneIsReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto shared = RenderTargetPtr(new RenderTarget());
    shared->setName(u8"shared");
    shared->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto promised_one = intrusive_ptr<ImageRef>(new ImageRef(u8"shared.color0"));
    promised_one->bind(shared, 0);

    auto fine = intrusive_ptr<RenderPass>(new RenderPass());
    fine->setName(u8"fine_owner");
    fine->setCamera(camera);
    fine->setRenderTarget(shared);
    fine->setOutput(promised_one);

    auto whole = intrusive_ptr<RenderPass>(new RenderPass());
    whole->setName(u8"whole_owner");
    whole->setCamera(camera);
    whole->setRenderTarget(shared);
    whole->setOutputTarget(shared);
    engine->addPass(fine, 0);
    engine->addPass(whole, 1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"fine_owner"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"whole_owner"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"shared"), vn::String::npos);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
}

/**
 * @brief A declared input image drives WHICH attachment the pass samples (no host-set index).
 *
 * The wire says "attachment 2 of the G-buffer" — which TARGET the pass reads — while the program says
 * which ATTACHMENT of it (its sampler binding, see BuiltinShaders::screenCopyProgram): the declaration
 * carries the target + image, the program carries the binding, and neither restates the other.
 */
TEST(RenderEngineTest, DeclaredImageDecidesWhichAttachmentIsSampled)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"gbuffer");
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->attachColor(RenderTarget::ColorFormat::RGBA16F);
    source->attachColor(RenderTarget::ColorFormat::RGBA16F);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer_pass");
    producer->setCamera(camera);
    producer->setRenderTarget(source);
    producer->setOutputTarget(source);
    engine->addPass(producer, -3);

    // Declares the third image and NOTHING else: no input name, no pass-side index. The DECLARATION
    // carries which target (and which image of it the host means); the PROGRAM carries which attachment
    // it reads, because in the fullscreen ABI that IS the sampler binding.
    auto sampled = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.specular"));
    sampled->bind(source, 2);
    auto preview = intrusive_ptr<ScreenPass>(new ScreenPass());
    preview->setName(u8"preview");
    preview->setCamera(camera);
    preview->setProgram(vn::graphics::screenCopyProgram(2));
    preview->addInput(sampled);
    engine->addPass(preview, 10);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_EQ(preview->sourceTarget(), source.get());
    EXPECT_EQ(backend->last_program_source, source.get());
}

/**
 * @brief A coarse declaration (a whole target) plus the program's binding names the attachment.
 *
 * A coarse declaration says "the whole of this target" (the unit the program path consumes); with the
 * copy path gone there is no pass-side attachment index any more, and none is needed: the program's
 * sampler binding says which attachment it reads (screenCopyProgram(1) reads attachment 1), and the
 * source has to provide it.
 */
TEST(RenderEngineTest, CoarseDeclarationAndTheProgramsBindingPickTheAttachment)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto baked = RenderTargetPtr(new RenderTarget());
    baked->setName(u8"baked");
    baked->attachColor(RenderTarget::ColorFormat::RGBA8);
    baked->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"bake");
    producer->setCamera(camera);
    producer->setRenderTarget(baked);
    engine->addPass(producer, -1);

    auto pip = intrusive_ptr<ScreenPass>(new ScreenPass());
    pip->setName(u8"pip");
    pip->setCamera(camera);
    pip->setProgram(vn::graphics::screenCopyProgram(1));
    pip->addInputTarget(baked);
    engine->addPass(pip, 100);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_EQ(pip->sourceTarget(), baked.get());
    EXPECT_EQ(backend->last_program_source, baked.get());
}

/**
 * @brief A declared input whose producer did not RUN this frame is reported, once per episode.
 *
 * The wiring check is declaration-only (a disabled producer still declares its wire — toggling a pass
 * off is not a wiring mistake), so only the frame knows that nothing filled the image. This is the
 * runtime half of D37, and it is what a consumer that "draws nothing" needs to be told.
 */
TEST(RenderEngineTest, DeclaredInputNotProducedThisFrameIsReportedOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"gbuffer");
    source->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer_pass");
    producer->setCamera(camera);
    producer->setRenderTarget(source);
    producer->setOutputTarget(source);
    engine->addPass(producer, -3);

    auto sampled = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.albedo"));
    sampled->bind(source, 0);
    auto preview = intrusive_ptr<ScreenPass>(new ScreenPass());
    preview->setName(u8"preview");
    preview->setCamera(camera);
    preview->setProgram(vn::graphics::screenCopyProgram());
    preview->addInput(sampled);
    engine->addPass(preview, 10);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);   // wired and produced: silent
    const int draws_before_pause = backend->program_draws;

    // The producer is paused (a legitimate runtime toggle, not a wiring mistake).
    producer->setEnabled(false);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"preview"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"GBuffer.albedo"), vn::String::npos);
    EXPECT_EQ(backend->program_draws, draws_before_pause);   // nothing to draw from

    // Still paused: one message for this episode, not one per frame.
    engine->frame(0.016);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // It runs again, and breaks again later: a new episode, reported again.
    producer->setEnabled(true);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    producer->setEnabled(false);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
}

/**
 * @brief A pass declaring both a name and an object resolves the wire ONCE.
 *
 * Names are the sugar over the declarations (design §14.3): a pass that states the same wire twice
 * must not get two entries (and must not be resolved twice), or "the first non-null input" would
 * depend on which layer the engine happened to consult first.
 */
TEST(RenderEngineTest, NameAndObjectDeclarationResolveTheWireOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"gbuffer");
    source->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"gbuffer_pass");
    producer->setCamera(camera);
    producer->setRenderTarget(source);
    producer->setOutputName(u8"GBuffer");
    producer->setOutputTarget(source);
    engine->addPass(producer, -3);

    auto sampled = intrusive_ptr<ImageRef>(new ImageRef(u8"GBuffer.albedo"));
    sampled->bind(source, 0);
    auto preview = intrusive_ptr<ScreenPass>(new ScreenPass());
    preview->setName(u8"preview");
    preview->setCamera(camera);
    preview->setProgram(vn::graphics::screenCopyProgram());
    preview->addInputName(u8"GBuffer");
    preview->addInput(sampled);
    engine->addPass(preview, 10);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_EQ(preview->sourceTarget(), source.get());
    EXPECT_EQ(backend->last_program_source, source.get());
}

/**
 * @brief A promise about a target the pass does not write is reported.
 *
 * A promise is a claim about the content a consumer gets ("this image is mine to hand out"), so it has
 * to be about what the pass draws into: promising a target the pass never writes is a claim about
 * nothing, and it makes the collision report fire on wires that do not exist. Both declaration layers
 * are checked (the whole target and the single image).
 */
TEST(RenderEngineTest, PromiseAboutATargetThePassDoesNotWriteIsReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto drawn_into = RenderTargetPtr(new RenderTarget());
    drawn_into->setName(u8"drawn_into");
    drawn_into->attachColor(RenderTarget::ColorFormat::RGBA8);
    // Two DIFFERENT targets neither pass writes — so the only reports are the mismatches (two passes
    // promising one target would be a collision, which is another rule).
    auto promised_coarse = RenderTargetPtr(new RenderTarget());
    promised_coarse->setName(u8"promised_coarse");
    promised_coarse->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto promised_fine = RenderTargetPtr(new RenderTarget());
    promised_fine->setName(u8"promised_fine");
    promised_fine->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto fine_image = intrusive_ptr<ImageRef>(new ImageRef(u8"promised_fine.color"));
    fine_image->bind(promised_fine, 0);

    auto coarse = intrusive_ptr<RenderPass>(new RenderPass());
    coarse->setName(u8"coarse_liar");
    coarse->setCamera(camera);
    coarse->setRenderTarget(drawn_into);
    coarse->setOutputTarget(promised_coarse);

    auto fine = intrusive_ptr<RenderPass>(new RenderPass());
    fine->setName(u8"fine_liar");
    fine->setCamera(camera);
    fine->setRenderTarget(drawn_into);
    fine->setOutput(fine_image);
    engine->addPass(coarse, 0);
    engine->addPass(fine, 1);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);   // one per declaration form
    ASSERT_EQ(received.size(), 2u);
    for (const auto& diagnostic : received) {
        EXPECT_EQ(diagnostic.category, DiagnosticCategory::ContentSkipped);
        EXPECT_NE(diagnostic.message.find(u8"drawn_into"), vn::String::npos);
    }
    EXPECT_NE(received[0].message.find(u8"promised_coarse"), vn::String::npos);
    EXPECT_NE(received[1].message.find(u8"promised_fine"), vn::String::npos);

    // The same broken promise next frame is the same episode: one message each, not one per frame.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);

    // Promising what it draws into is what the rule asks for: silent.
    coarse->setOutputTarget(drawn_into);
    fine->setRenderTarget(promised_fine);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);

    // Breaking it again is a new episode.
    coarse->setOutputTarget(promised_coarse);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 3u);
}

/**
 * @brief A host's publish() is a STANDING binding, not a per-frame publication.
 *
 * The named-output registry is cleared at the start of every frame so a pass that stopped running
 * stops publishing — that reasoning is about PASSES (their producer must run each frame). A host
 * binding has no producer to re-publish it and no per-frame hook to do it from, so wiping it makes
 * the documented capability ("this lets application code publish extra targets") unusable: the
 * binding dies before any consumer could resolve it.
 */
TEST(RenderEngineTest, HostPublishedTargetSurvivesTheFrame)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    // A target the host filled itself (an imported frame, an editor preview): no pass draws into it.
    auto external = RenderTargetPtr(new RenderTarget());
    external->setName(u8"external");
    external->attachColor(RenderTarget::ColorFormat::RGBA8);
    engine->publish(u8"External", external);

    // A consumer that resolves the name.
    auto by_name = intrusive_ptr<ScreenPass>(new ScreenPass());
    by_name->setName(u8"by_name");
    by_name->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    by_name->setProgram(vn::graphics::screenCopyProgram());
    by_name->addInputName(u8"External");
    engine->addPass(by_name, 0);

    // A consumer that declares the image it samples (the object path checks "was it produced this
    // frame", and a host binding is available every frame).
    auto declared = intrusive_ptr<ImageRef>(new ImageRef(u8"External.color"));
    declared->bind(external, 0);
    auto by_object = intrusive_ptr<ScreenPass>(new ScreenPass());
    by_object->setName(u8"by_object");
    by_object->setCamera(camera);
    by_object->setProgram(vn::graphics::screenCopyProgram());
    by_object->addInput(declared);
    engine->addPass(by_object, 1);

    for (int frame_index = 0; frame_index < 3; ++frame_index) {
        engine->frame(0.016);
        EXPECT_EQ(engine->diagnosticCount(), 0u) << "frame " << frame_index;
    }
    EXPECT_EQ(engine->resolve(u8"External"), external.get());
    EXPECT_EQ(backend->last_program_source, external.get());

    // unpublish() is what ends a host binding.
    engine->unpublish(u8"External");
    EXPECT_EQ(engine->resolve(u8"External"), nullptr);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);   // both consumers now have nothing
    EXPECT_TRUE(received.empty() || received.size() >= 2u);
}

/**
 * @brief A ScreenPass that declares no input at all is reported, once per episode.
 *
 * A ScreenPass samples exactly one input and returns without it, so with no input declared it can
 * never draw — the frame silently loses its content. "No input" is not "an input whose producer is
 * missing": resolvePassInputs only watches declared names, so this would otherwise be invisible.
 */
/**
 * @brief A ScreenPass that can never draw is reported, not left silent.
 *
 * The fullscreen program path builds the pass' view from its camera (and pushes the scene's lights
 * through that view space), so a ScreenPass with a program and NO camera draws nothing at all
 * (ScreenPass::execute returns before asking the backend for anything). The host would see a
 * post-process that never appears, with no reason for it — a wiring-time fact like the input-less
 * case below, and static like it: the fix is setCamera().
 */
TEST(RenderEngineTest, ScreenPassWithAProgramAndNoCameraIsReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"scene");
    producer->setCamera(camera);
    producer->setRenderTarget(RenderTargetPtr(new RenderTarget()));
    producer->setOutputName(u8"SceneColor");
    engine->addPass(producer, -1);

    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    ShaderStage stage;
    stage.type   = ShaderStageType::Fragment;
    stage.source = u8"#version 450\nvoid main() {}\n";
    program->addStage(stage);

    auto light = intrusive_ptr<ScreenPass>(new ScreenPass());
    light->setName(u8"light");
    light->addInputName(u8"SceneColor");
    light->setProgram(program);
    // NO setCamera: the program path has no view to build.
    engine->addPass(light, 1);

    const int draws_before = backend->program_draws;
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"light"), vn::String::npos);
    EXPECT_NE(received[0].message.find(u8"camera"), vn::String::npos);
    // The report describes what actually happens: nothing was drawn through the program.
    EXPECT_EQ(backend->program_draws - draws_before, 0);

    // Same problem next frame: one episode, one message.
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // Giving it a camera ends the episode, and the pass draws.
    light->setCamera(camera);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    EXPECT_EQ(backend->program_draws - draws_before, 1);
}

TEST(RenderEngineTest, ScreenPassWithoutAnyInputIsReportedOnce)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);

    auto consumer = intrusive_ptr<ScreenPass>(new ScreenPass());
    consumer->setName(u8"overlay");
    consumer->setCamera(camera);
    // A drawable ScreenPass names its program (see ScreenPass): this test is about the wiring, so the
    // pass has to be valid apart from the one thing under test.
    consumer->setProgram(vn::graphics::screenCopyProgram());
    engine->addPass(consumer, 0);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_NE(received[0].message.find(u8"overlay"), vn::String::npos);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // An input appears (a bound image with a producer registered earlier): the pass can draw, so
    // nothing more to say. The producer matters — an input image nobody produces is its own
    // report (see the declared-input-image tests below).
    auto source = RenderTargetPtr(new RenderTarget());
    auto image  = intrusive_ptr<ImageRef>(new ImageRef(u8"SceneColor"));
    image->bind(source, 0);
    consumer->addInput(image);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setName(u8"fill");
    producer->setCamera(camera);
    producer->setRenderTarget(source);
    producer->setOutput(image);
    engine->addPass(producer, -1);
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // Its input leaves again: the same problem, so it is reported again.
    consumer->clearInputs();
    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 2u);
    EXPECT_EQ(received.size(), 2u);
}

/**
 * @brief A base RenderPass with no input is NOT reported: drawing nothing is what it is for.
 *
 * Only a ScreenPass samples an input; a scene pass draws its bound content and needs no input at
 * all, so the input-less rule must not touch it (otherwise every scene pass would warn).
 */
TEST(RenderEngineTest, ScenePassWithoutInputIsNotReported)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setName(u8"scene");
    pass->setCamera(camera);
    pass->setRenderTarget(RenderTargetPtr(new RenderTarget()));
    engine->addPass(pass, 0);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(received.empty());
}

TEST(RenderEngineTest, PublishingOneTargetUnderOneNameTwiceIsNotACollision)
{
    std::vector<RenderDiagnostic> received;
    auto                          backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto                          engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    engine->setDiagnosticSink([&received](const RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    ASSERT_TRUE(engine->initialize());

    auto camera        = intrusive_ptr<Camera>(new Camera());
    setupLookAtCamera(*camera);
    auto shared_target = RenderTargetPtr(new RenderTarget());

    const auto make_producer = [&camera, &shared_target](const vn::String& name, int order) {
        auto pass = intrusive_ptr<RenderPass>(new RenderPass());
        pass->setName(name);
        pass->setCamera(camera);
        pass->setRenderTarget(shared_target);
        pass->setOutputName(u8"shared");
        return std::make_pair(pass, order);
    };
    auto first  = make_producer(u8"first", -2);
    auto second = make_producer(u8"second", -1);
    engine->addPass(first.first, first.second);
    engine->addPass(second.first, second.second);

    engine->frame(0.016);
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(received.empty());
}

// ============ Measurement recipe: what one collection costs ============

TEST(SceneTest, MeasureWhatOneCollectionCostsWithAMovingCamera)
{
    // A RECIPE, not a gate: it answers the question the B4 row in the backend's design log
    // (.ai/design/vsg-reimplementation.md §11.16bw) could not - "the demo's collection is a memo hit or ~42
    // commands, so the cost is unmeasurable there". Nothing here asserts a machine-dependent number; the
    // figures below are printed so they can be recorded in the design log, and the asserted facts are the
    // machine-independent ones: a MOVED camera collects again, a STILL one reuses the shared list, and the
    // command list is the same size either way.
    //
    // Run it where the numbers matter (Release builds the walk inlined):
    //   ./build-release/bin/test_graphics --gtest_filter='*MeasureWhatOneCollectionCosts*'
    std::printf("\n");   // keep the measurements on their own lines
    for (const std::size_t drawables : { std::size_t{ 200 }, std::size_t{ 2000 } }) {
        Scene scene;
        intrusive_ptr<Group> root = setIdentityRoot(scene);
        // A grid of unit triangles five units in front of the camera: all inside the frustum, at DIFFERENT
        // distances (so the sort has real work), and small enough that the walk's culling cannot remove any.
        for (std::size_t index = 0; index < drawables; ++index) {
            const double x = static_cast<double>(index % 50U) * 0.04 - 1.0;
            const double y = static_cast<double>((index / 50U) % 50U) * 0.04 - 1.0;
            root->addChild(makeTriangleNode(Vec3d(x, y, -5.0), nullptr));
        }

        Camera camera;
        setupLookAtCamera(camera);
        // The memo lives INSIDE one content frame (see Scene::setContentFrame: a new token drops the previous
        // frame's lists), so the recipe opens one and keeps it: what is measured is the within-frame reuse -
        // exactly the question "the demo's next frame collects again, what does that cost".
        scene.setContentFrame(1);
        const auto moving = [&camera](int step) {
            // A moved EYE: the memo keys on (revision, eye, view projection), so every one of these misses.
            camera.setViewMatrixAsLookAt(Vec3d(0.001 * static_cast<double>(step), 0, 5), Vec3d(0, 0, 0),
                                         Vec3d(0, 1, 0));
        };

        moving(0);
        const std::uint64_t collects_before = scene.contentCollectCount();
        const auto          warm            = scene.collectRenderCommandsShared(&camera);
        ASSERT_NE(warm, nullptr);
        const std::size_t commands = warm->size();
        EXPECT_EQ(scene.contentCollectCount(), collects_before + 1U);

        constexpr int kRepetitions = 10;
        const auto    began        = std::chrono::steady_clock::now();
        const void*   last         = nullptr;
        for (int step = 1; step <= kRepetitions; ++step) {
            moving(step);
            const auto collected = scene.collectRenderCommandsShared(&camera);
            ASSERT_NE(collected, nullptr);
            EXPECT_NE(collected.get(), warm.get()) << "a moved camera cannot reuse the shared list";
            last = collected.get();
        }
        const void* first_of_moving = last;
        const auto  moving_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began)
                .count() /
            kRepetitions;
        EXPECT_EQ(scene.contentCollectCount(), collects_before + 1U + static_cast<std::uint64_t>(kRepetitions))
            << "every moved view is a real walk";

        const std::uint64_t reuses_before = scene.contentCollectReuseCount();
        const auto          still_began   = std::chrono::steady_clock::now();
        const auto          still_first   = scene.collectRenderCommandsShared(&camera);
        for (int step = 1; step < kRepetitions; ++step) {
            const auto again = scene.collectRenderCommandsShared(&camera);
            ASSERT_NE(again, nullptr);
            EXPECT_EQ(again.get(), still_first.get()) << "a still camera reuses the shared list";
        }
        const auto still_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - still_began)
                .count() /
            kRepetitions;
        EXPECT_GE(scene.contentCollectReuseCount(), reuses_before + static_cast<std::uint64_t>(kRepetitions - 1))
            << "a still camera is the memo's whole point";

        EXPECT_EQ(still_first->size(), commands) << "both views collect the same commands";
        EXPECT_NE(first_of_moving, nullptr);
        std::printf("[measure] drawables=%zu commands=%zu moving=%lld us/collect still=%lld us/collect (%d of "
                    "each)\n",
                    drawables, commands, static_cast<long long>(moving_us), static_cast<long long>(still_us),
                    kRepetitions);
        std::fflush(stdout);
    }
}

TEST(SceneMutationTest, ReparentingMovesTheCommandAndBothSubtreeBoxes)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);

    auto left = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    left->setMatrix(vn::math::translate(Vec3d(-2.0, 0.0, 0.0)));
    auto right = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    right->setMatrix(vn::math::translate(Vec3d(2.0, 0.0, 0.0)));
    root->addChild(left);
    root->addChild(right);

    auto mover = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"mover");
    left->addChild(mover);

    Camera cam;
    setupLookAtCamera(cam);

    // The world-space box is the end result; the cache witness is the
    // recompute counter read below (the collection walk is what consumes the
    // per-node cached boxes).
    const Aabbd before = scene.boundingBox();
    ASSERT_TRUE(before.isValid());
    EXPECT_NEAR(before.min().x, -2.0, 1e-9);
    EXPECT_NEAR(before.max().x, -1.0, 1e-9);

    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_NEAR(commands[0].modelMatrix.element(0, 3), -2.0, 1e-9);
    const std::uint64_t left_seeded  = left->boundsRecomputeCount();
    const std::uint64_t right_seeded = right->boundsRecomputeCount();

    // The only thing that changes: which matrix sits above the mover.
    right->addChild(mover);

    EXPECT_TRUE(left->children().empty());
    EXPECT_EQ(right->children().size(), 1u);
    EXPECT_EQ(mover->parent(), right.get());

    const Aabbd after = scene.boundingBox();
    ASSERT_TRUE(after.isValid());
    EXPECT_NEAR(after.min().x, 2.0, 1e-9);
    EXPECT_NEAR(after.max().x, 3.0, 1e-9);

    commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_NEAR(commands[0].modelMatrix.element(0, 3), 2.0, 1e-9);

    // Both unions are re-derived: the old parent loses a child, the new one
    // gains it, and each announcement must reach its own chain (the per-node
    // box cache is served by stamp, so a missed one stays stale for every
    // later walk).
    EXPECT_GT(left->boundsRecomputeCount(), left_seeded) << "the OLD parent's union is re-derived";
    EXPECT_GT(right->boundsRecomputeCount(), right_seeded) << "the NEW parent's union is re-derived";
}

TEST(SceneMutationTest, ReparentingSwapsWhichStateOverridesApply)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);

    auto material_a = MaterialPtr(new Material());
    auto material_b = MaterialPtr(new Material());
    auto program_a  = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program_a->setName(u8"program-a");
    auto program_b = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program_b->setName(u8"program-b");

    auto state_a = intrusive_ptr<StateNode>(new StateNode());
    state_a->setMaterial(material_a);
    state_a->setProgram(program_a);
    state_a->setOpacity(0.25f);
    auto state_b = intrusive_ptr<StateNode>(new StateNode());
    state_b->setMaterial(material_b);
    state_b->setProgram(program_b);
    state_b->setOpacity(0.5f);

    auto holder = intrusive_ptr<Group>(new Group());
    holder->addChild(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    state_a->addChild(holder);
    root->addChild(state_a);
    root->addChild(state_b);

    Camera cam;
    setupLookAtCamera(cam);

    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), material_a.get());
    EXPECT_EQ(commands[0].program.get(), program_a.get());
    EXPECT_NEAR(commands[0].opacity, 0.25f, 1e-5f);

    // The override chain is re-resolved every collection, so the move changes
    // material, program and opacity together.
    state_b->addChild(holder);
    commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), material_b.get());
    EXPECT_EQ(commands[0].program.get(), program_b.get());
    EXPECT_NEAR(commands[0].opacity, 0.5f, 1e-5f);

    // And back: nothing of the other subtree sticks.
    state_a->addChild(holder);
    commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), material_a.get());
    EXPECT_EQ(commands[0].program.get(), program_a.get());
    EXPECT_NEAR(commands[0].opacity, 0.25f, 1e-5f);
}

TEST(SceneMutationTest, SwappingMaterialAndProgramTakesEffectOnTheNextCollection)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);
    auto  node = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri");
    root->addChild(node);
    auto* geom = dynamic_cast<Geometry*>(node->children().front().get());
    ASSERT_NE(geom, nullptr);

    Camera cam;
    setupLookAtCamera(cam);

    auto material_x = MaterialPtr(new Material());
    auto material_y = MaterialPtr(new Material());
    auto program_x  = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program_x->setName(u8"program-x");
    auto program_y = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program_y->setName(u8"program-y");

    geom->setMaterial(material_x);
    geom->setProgram(program_x);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), material_x.get());
    EXPECT_EQ(commands[0].program.get(), program_x.get());

    // A swap on the same drawable: the next collection carries the new
    // identities, not the ones it started with.
    geom->setMaterial(material_y);
    geom->setProgram(program_y);
    commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material.get(), material_y.get());
    EXPECT_EQ(commands[0].program.get(), program_y.get());

    // And clearing both falls back to the inherited defaults.
    geom->setMaterial(nullptr);
    geom->setProgram(nullptr);
    commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].material, nullptr);
    EXPECT_EQ(commands[0].program, nullptr);
}

TEST(SceneMutationTest, RemovingASubtreeDropsItsCommandsAndShrinksTheBox)
{
    Scene scene;
    auto  root = setIdentityRoot(scene);

    auto keeper  = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"keeper");
    auto removed = makeTriangleNode(Vec3d(-2, 0, -3), nullptr, u8"removed");
    root->addChild(keeper);
    root->addChild(removed);

    auto* keeper_geom  = dynamic_cast<Geometry*>(keeper->children().front().get());
    auto* removed_geom = dynamic_cast<Geometry*>(removed->children().front().get());
    ASSERT_NE(keeper_geom, nullptr);
    ASSERT_NE(removed_geom, nullptr);

    Camera cam;
    setupLookAtCamera(cam);

    // The world-space box after the removal is the end result; the cache
    // witness is the recompute counter read below.
    const Aabbd before = scene.boundingBox();
    ASSERT_TRUE(before.isValid());
    EXPECT_NEAR(before.min().x, -2.0, 1e-9);

    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 2u);
    const std::uint64_t root_seeded = root->boundsRecomputeCount();

    root->removeChild(removed.get());
    EXPECT_EQ(root->childrenRef().size(), 1u);
    EXPECT_EQ(removed->parent(), nullptr);

    // The box shrinks with the removal, and the command disappears.
    const Aabbd after = scene.boundingBox();
    ASSERT_TRUE(after.isValid());
    EXPECT_NEAR(after.min().x, 0.0, 1e-9);
    EXPECT_NEAR(after.max().x, 1.0, 1e-9);

    commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry.get(), keeper_geom);

    // The removal announces on the OLD chain, so the parent's union is
    // re-derived instead of being served stale from the per-node cache.
    EXPECT_GT(root->boundsRecomputeCount(), root_seeded) << "the removal announces on the old chain";

    // Removal is not destruction: the detached subtree can come back.
    root->addChild(removed);
    commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 2u);
    bool removed_back = false;
    for (const RenderCommand& command : commands) {
        removed_back = removed_back || command.geometry.get() == removed_geom;
    }
    EXPECT_TRUE(removed_back);
    EXPECT_NEAR(scene.boundingBox().min().x, -2.0, 1e-9);
}
