#include "SdkPixelFixture.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Point3.hpp>
#include <vine/vsg/internal/DeviceProbe.hpp>

using vn::graphics::Camera;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::MatrixTransform;
using vn::graphics::RenderDiagnostic;
using vn::graphics::RenderTarget;
using vn::graphics::Scene;
using vn::graphics::StateNode;

namespace
{
/// @brief Half-extent of the quad the fixture draws, in world units.
constexpr double kQuadHalf = 0.9;

/// @brief The plane the quad lies in: in front of the fixture's camera, facing it.
constexpr double kQuadZ = 0.5;

/// @brief The scene every case here draws: one flat, bright, unlit quad in world space.
///
/// WHY A QUAD. The pictures this fixture compares are "surfaces" against "the edges of those surfaces", so the
/// content has to cover a real part of the frame (a sliver would make every count noisy) and its colour has to
/// be tellable from the clear colour. The frame is cleared to BLACK and this surface is WHITE, so "covered" is
/// `PixelProbe::nonBlackPixels()` - no assertion here has to know a colour space, an encoding or a byte order.
///
/// WHY POSITIONS AND NORMALS. The channels a geometry declares are the ones the built-in programs read
/// (position at location 0, normal at 1 - see usage.md §3.8), and a geometry that declares fewer channels than
/// the program reads is a draw the backend refuses rather than a picture with a warning. The texcoord channel
/// is deliberately absent: none of the shading modes used here samples a map.
///
/// @return The scene, ready to be handed to a view or to a pass.
vn::intrusive_ptr<Scene> makeQuadScene()
{
    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(
        std::vector<float>{ static_cast<float>(-kQuadHalf), static_cast<float>(-kQuadHalf),
                            static_cast<float>(kQuadZ),  static_cast<float>(kQuadHalf),
                            static_cast<float>(-kQuadHalf), static_cast<float>(kQuadZ),
                            static_cast<float>(kQuadHalf),  static_cast<float>(kQuadHalf),
                            static_cast<float>(kQuadZ),  static_cast<float>(-kQuadHalf),
                            static_cast<float>(kQuadHalf),  static_cast<float>(kQuadZ) })));
    geometry->setNormals(vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                                  0.0F, 1.0F })));
    geometry->setIndices(vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F));

    const vn::intrusive_ptr<StateNode> state(new StateNode());
    state->setMaterial(material);
    state->addChild(geometry);

    const vn::intrusive_ptr<MatrixTransform> transform(new MatrixTransform());
    transform->setMatrix(vn::math::Mat4d());
    transform->addChild(state);

    const vn::intrusive_ptr<Scene> scene(new Scene());
    scene->setRoot(transform);
    return scene;
}
}  // namespace

bool SdkPixelFixture::available()
{
    const vn::vsg::api::ProbeResult probed = vn::vsg::api::probePhysicalDevices();
    return probed.ok && probed.usableCount() != 0U;
}

SdkPixelFixture::SdkPixelFixture() = default;

SdkPixelFixture::~SdkPixelFixture()
{
    // Hand the target back BEFORE the session goes: while the session is up the backend holds the announced
    // target (by pointer, see RenderBackend::setRenderTarget), so a target destroyed under a live backend is
    // a dangling handle rather than a released resource.
    if (backend != nullptr && backend->initialized()) {
        backend->setRenderTarget(nullptr);
    }
    if (engine != nullptr) {
        engine->shutdown();
    }
}

bool SdkPixelFixture::up()
{
    backend = vn::intrusive_ptr<vn::vsg::VsgBackend>(new vn::vsg::VsgBackend());
    engine  = vn::intrusive_ptr<vn::graphics::RenderEngine>(new vn::graphics::RenderEngine());
    engine->setBackend(backend);
    engine->setDiagnosticSink([](const RenderDiagnostic& diagnostic) {
        std::printf("[sdk-pixels] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    std::string(reinterpret_cast<const char*>(diagnostic.message.data()),
                                diagnostic.message.size())
                        .c_str());
    });

    // No window handle is announced: this backend then owns the window it brings the session up on. That
    // window is never read - the picture this fixture probes comes from the target below - so a case here
    // depends on the device and the content path, not on the platform's surface.
    if (!engine->initialize()) {
        return false;
    }

    // The picture's SHAPE is the target's: a pass that draws into this target projects for a square frame
    // whatever surface is behind it (see RenderBackend::viewAspect), which is what makes a 64x64 picture a
    // measurement rather than a window screenshot.
    target = vn::intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(vn::String(u8"sdk_pixels"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kSide, kSide);
    backend->setRenderTarget(target.get());

    scene = makeQuadScene();

    // The fixture's own camera: it frames the quad in an orthographic window of [-1, 1] squared, so the quad
    // covers 81% of whatever is drawn through it. Cases that hand the camera to a pass use this one; a case
    // that draws through a SceneView uses the view's own camera and its manipulator instead.
    camera = vn::intrusive_ptr<Camera>(new Camera());
    camera->setViewMatrixAsLookAt(vn::math::Vec3d(0.0, 0.0, 1.5), vn::math::Vec3d(0.0, 0.0, 0.0),
                                  vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);
    return true;
}

vn::graphics::backend::PixelProbe SdkPixelFixture::read()
{
    engine->frame(0.0);

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Ok;
    if (!backend->readColorBuffer(target.get(), 0, pixels, &why)) {
        std::printf("[sdk-pixels] the readback was refused (why=%d)\n", static_cast<int>(why));
        return vn::graphics::backend::PixelProbe(0, 0, {});
    }
    return vn::graphics::backend::PixelProbe(kSide, kSide, std::move(pixels));
}
