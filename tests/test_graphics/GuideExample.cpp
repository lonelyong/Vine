// The compiled copy of the module guide's FIRST example (`docs/guides/graphics.md`, section 3.1).
//
// WHY A TRANSLATION UNIT AND NOT A SNIPPET: the first example a host reads was the one piece of the
// guide nothing compiled, and it had rotted by three public-API changes - it named `make_intrusive`
// unqualified (it lives in `vn`, not in `vn::graphics`), it constructed a `RenderArea` directly (the
// constructor is private: `RenderEngine::createArea` is the only way to get one), and it passed a
// delta time to a `frame()` that takes none. A snippet is cheap to leave behind; a file the build
// compiles is not. `scripts/check_doc_symbols.py` additionally requires every statement the guide's
// block carries to appear here, so the two cannot drift apart again.
//
// WHAT RUNS AND WHAT ONLY COMPILES:
// - `guideScene()` / `guideCamera()` (steps 1-3) are EXECUTED by `GuideExampleTest`: they need no
//   device, and the case proves the documented scene really collects one draw item.
// - `guideHelloTriangle()` (steps 4-5) is COMPILED and never called - a window handle and a loaded
//   backend plug-in are a host's business, not a unit test's.
// - `guideQuickGeometry()` (the quick quad / box recipe beside §3.1) is COMPILED and never called.

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/PassChain.hpp>
#include <vine/graphics/RenderArea.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/StateNode.hpp>

#include <vine/models/Array.hpp>
#include <vine/models/Box.hpp>
#include <vine/models/QuadMesh.hpp>

/// The guide example's statements, in the guide's own order.
namespace guide_example
{
using namespace vn::graphics;

/**
 * @brief Steps 1-2 of the guide example: one triangle in a scene graph with ambient and directional light.
 *
 * @details
 * - A transform -> state -> geometry chain is the documented convention: placement, then look, then leaf.
 * - `setPositions` on a primitive-less geometry also states its one draw, so the mesh needs no `addPrimitive`.
 *
 * @return The scene the example's steps 1-2 build.
 */
ScenePtr guideScene()
{
    vn::models::Vec3fArray positions = {
        vn::math::Vec3f(0.0f, 0.0f, 0.0f),
        vn::math::Vec3f(1.0f, 0.0f, 0.0f),
        vn::math::Vec3f(0.0f, 1.0f, 0.0f),
    };
    GeometryPtr geometry(new Geometry());
    geometry->setPositions(packAttribute(positions));

    MaterialPtr material(new Material());
    material->setBaseColor(vn::Colorf(0.8f, 0.4f, 0.2f, 1.0f));

    auto state = StateNodePtr(new StateNode());
    state->setMaterial(material);
    state->addChild(geometry);

    auto transform = MatrixTransformPtr(new MatrixTransform());
    transform->setMatrix(Mat4d());
    transform->addChild(state);

    auto scene = vn::make_intrusive<Scene>();
    scene->setRoot(transform);
    scene->addLight(Light::createAmbient());
    scene->addLight(Light::createDirectional({ 0.0, 0.0, -1.0 }));

    return scene;
}

/**
 * @brief Step 3 of the guide example: the camera the triangle is seen with.
 *
 * @details
 * - The projection's SHAPE is the camera's own state (`setPerspective` + `setAspect`), which is why the
 *   example states both: one camera is one picture.
 *
 * @return The camera the example's step 3 builds.
 */
CameraPtr guideCamera()
{
    CameraPtr camera(new Camera());
    camera->lookAt({ 0.0, 0.0, 3.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 });
    camera->setPerspective(45.0, 0.1, 100.0);
    camera->setAspect(16.0 / 9.0);

    return camera;
}

/**
 * @brief The quick shapes a host reaches for before authoring vertices itself: one quad, one box.
 *
 * @details
 * - `QuadMesh` is four corners plus four corner indices; `geometryFromShape()` turns it into two triangles.
 * - `Box` is the analytic solid: the six flat faces come from geom's tessellation, so the caller names an extent
 *   and no vertex at all.
 * - Both conversions state the indexed triangle draw themselves, which is why neither shape has to replace the
 *   default draw that `setPositions` states on a hand-built geometry.
 *
 * @return The two geometries (compiled, not called: the guide prints them).
 */
std::pair<GeometryPtr, GeometryPtr> guideQuickGeometry()
{
    auto quad = vn::make_intrusive<vn::models::QuadMesh>();
    quad->setPositions({ vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 1.0f, 0.0f), vn::math::Vec3f(0.0f, 1.0f, 0.0f) });
    quad->setIndices({ 0u, 1u, 2u, 3u });
    const GeometryPtr quad_geometry = geometryFromShape(*quad);

    auto box = vn::make_intrusive<vn::models::Box>(2.0f, 2.0f, 2.0f);
    const GeometryPtr box_geometry = geometryFromShape(*box);
    return { quad_geometry, box_geometry };
}

/**
 * @brief Steps 4-5 of the guide example: device, area, chain and the frame loop (compiled, not called).
 *
 * @details
 * - The order is the one the contracts require: the device is brought up before the engine attaches to
 *   it, the area is asked of the engine (never constructed), and the size is announced on the area once -
 *   the frame context, the window half and every presented view all read that one number.
 * - `frame()` takes NO time: the engine does not own the clock. A host that animates turns its own
 *   elapsed time into a transform (see `PositionAttitudeTransform`) before the frame runs.
 *
 * @param window_handle Native window handle the device builds a surface target for.
 * @param width         Surface width in logical pixels.
 * @param height        Surface height in logical pixels.
 * @param dpr           Device pixels per logical pixel.
 * @return true when the whole sequence came up; false at the first contract that refused.
 */
bool guideHelloTriangle(void* window_handle, int width, int height, double dpr)
{
    auto scene  = guideScene();
    auto camera = guideCamera();

    auto engine = vn::make_intrusive<RenderEngine>();
    engine->setDevice(RenderBackendRegistry::instance().create(u8"vsg"));

    auto area = engine->createArea();
    if (area == nullptr) { return false; }
    vn::graphics::SurfaceTargetResult why = vn::graphics::SurfaceTargetResult::Ok;
    vn::raw_ptr<vn::graphics::WindowPresentTarget> target = engine->device()->createSurfaceTarget(window_handle, &why);
    if (target == nullptr) { return false; }
    area->bindTarget(target);
    area->resize(width, height, dpr);

    if (!engine->device()->initialize()) { return false; }
    if (!engine->initialize()) { return false; }

    ChainDesc options;
    options.content = scene;
    options.camera  = camera.get();
    auto pipeline = PassChain::build(engine.get(), options);
    if (pipeline == nullptr) { return false; }

    pipeline->resize(width, height, dpr);
    engine->frame();

    engine->shutdown();
    return true;
}

} // namespace guide_example

TEST(GuideExampleTest, TheDocumentedSceneAndCameraCollectTheOneTriangle)
{
    auto scene  = guide_example::guideScene();
    auto camera = guide_example::guideCamera();

    auto items = scene->collectDrawItems(camera.get(), vn::graphics::kAllLayers, nullptr);

    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->size(), std::size_t{ 1 });
}
