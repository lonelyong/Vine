/**
 * @brief M9: a SHADOW MAP - a depth-only pass this backend records, and the engine's own forward program sampling it.
 *
 * WHY THIS IS THE HARDEST SHAPE THE BACKEND HAS HAD. Three facts have to agree, and each of them is somebody else's
 * statement:
 *   * the TARGET says it is a map (`RenderTarget::setShadowOf`) and how to read it (`setProducerViewProjection`), and it
 *     has NO colour attachment - so the pass that writes it is a depth-only render pass with a depth-only pipeline;
 *   * the CONSUMING pass declares the map as an input, and this backend has to move the map's depth image out of the
 *     attachment layout (an image barrier is not legal inside a render pass, so it happens before the pass begins);
 *   * the LIGHT the map belongs to must still be a caster among the lights the drawing call announced: `VineShadowBlock`
 *     is packed per call from that match (its slot, its own bias, and `light clip <- world` times `world <- view`).
 *
 * WHAT THE PICTURES SAY (the acceptance `graphics-rendering.md` T5 states). With a casting light the ground behind the
 * caster is darker than the ground beside it; with the SAME frame and the light's switch off, the picture is
 * BYTE-IDENTICAL to a frame that declared no map at all - so "the switch decided it" and "the map was ignored" cannot
 * be confused with "the shading changed".
 *
 * The scene is stated in CLIP space on purpose (identity camera and identity producer matrix): every number below is
 * then the value the hardware compares, with no camera derivation in the way.
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/String.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameStats.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ImageSource.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>

using vn::BufferSlice;
using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DepthMode;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::ImageSource;
using vn::graphics::Light;
using vn::graphics::LightValue;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 64;

/// @brief The camera both passes draw through: identities, so clip space IS the picture.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    return camera;
}

/// @brief The 64x64 RGBA8 + D24 target the picture is read from.
[[nodiscard]] intrusive_ptr<RenderTarget> makeColorTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_shadow_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief A 64x64 DEPTH-ONLY target that states whose map it is and how to read it (the engine's shadow-map shape).
[[nodiscard]] intrusive_ptr<RenderTarget> makeShadowMap(const intrusive_ptr<Light>& light)
{
    auto map = intrusive_ptr<RenderTarget>(new RenderTarget());
    map->setName(String(u8"vk_shadow_map"));
    map->attachDepth(RenderTarget::DepthFormat::D24);
    map->setSize(kTargetSize, kTargetSize);
    map->setShadowOf(light);
    // The producer's own matrix, in the SDK's clip convention: the identity, because this scene's light camera is the
    // camera it was rasterised with (see the file note).
    map->setProducerViewProjection(vn::math::Mat4d{});
    return map;
}

/// @brief A light that travels along -z, so a surface facing +z is fully lit (`vnEvaluateLight` negates the direction).
[[nodiscard]] intrusive_ptr<Light> makeCastingLight()
{
    auto light = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    light->setColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    light->setIntensity(1.0F);
    light->setCastShadow(true);
    return light;
}

/// @brief An axis-aligned quad at depth @p z, spanning [x0, x1] on x and the whole height, facing +z.
[[nodiscard]] intrusive_ptr<Geometry>
makeQuad(float x0, float x1, float z)
{
    // Two triangles, six vertices, no index buffer (a vertex-only draw is a shape this backend states explicitly).
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        x0, -1.0F, z,
        x1, -1.0F, z,
        x1,  1.0F, z,
        x0, -1.0F, z,
        x1,  1.0F, z,
        x0,  1.0F, z,
    })));
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief One command: @p geometry through @p program, shaded with @p material.
[[nodiscard]] RenderCommand commandOf(const intrusive_ptr<Geometry>& geometry,
                                      const intrusive_ptr<ShaderProgram>& program,
                                      const intrusive_ptr<Material>& material)
{
    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    return command;
}

/// @brief One scene drawing call: @p commands through the identity camera, optionally announcing @p lights.
[[nodiscard]] DrawDescription sceneDraw(std::shared_ptr<const std::vector<RenderCommand>> commands,
                                        std::optional<std::vector<LightValue>>              lights = std::nullopt)
{
    DrawDescription draw;
    draw.kind        = DrawDescription::Kind::Scene;
    draw.camera      = identityCamera();
    draw.commands    = std::move(commands);
    draw.light_values = std::move(lights);
    return draw;
}

/// @brief What a case's two-pass frame is made of.
struct Scene {
    intrusive_ptr<RenderTarget>   target;    // The picture that is read back.
    intrusive_ptr<RenderTarget>   map;       // The shadow map (built here, but only DECLARED when `declared` is true).
    intrusive_ptr<ShaderProgram>  program;   // The engine's forward (lit) program.
    intrusive_ptr<Material>       material;  // White, so the light terms are the only thing that moves a pixel.
    intrusive_ptr<Light>          light;     // The caster.
    intrusive_ptr<Geometry>       ground;    // The receiving surface: the whole screen at z = 0.5.
    intrusive_ptr<Geometry>       occluder;  // The caster: the left strip at z = 0.0.
    intrusive_ptr<vn::graphics::RenderPass> shadow_pass; // Pass identity of the depth-only pass.
    intrusive_ptr<vn::graphics::RenderPass> main_pass;   // Pass identity of the shading pass.
};

/// @brief Builds the scene (one caster, one receiver, everything stated in clip space).
[[nodiscard]] Scene makeScene()
{
    Scene scene;
    scene.target   = makeColorTarget();
    scene.light    = makeCastingLight();
    scene.map      = makeShadowMap(scene.light);
    scene.program  = vn::graphics::forwardProgram();
    scene.material = intrusive_ptr<Material>(new Material());
    scene.material->setBaseColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    // The receiver sits BEHIND the caster along the light (-z travels from the camera into the screen), so the strip the
    // caster covers must come out of shadow.
    scene.ground    = makeQuad(-1.0F, 1.0F, 0.5F);
    scene.occluder  = makeQuad(-1.0F, -0.25F, 0.0F);
    scene.shadow_pass = intrusive_ptr<vn::graphics::RenderPass>(new vn::graphics::RenderPass());
    scene.main_pass   = intrusive_ptr<vn::graphics::RenderPass>(new vn::graphics::RenderPass());
    return scene;
}

/**
 * @brief The frame: the depth-only pass that writes the map, then the shading pass that samples it.
 *
 * @param scene    The scene (borrowed for the call).
 * @param declare  Whether the shading pass DECLARES the map as an input (false: the same frame without a shadow).
 * @param casting  Whether the announced light is still a caster (false: the map is declared but cannot shade).
 * @return The frame value.
 */
[[nodiscard]] FrameDescription shadowFrame(const Scene& scene, bool declare, bool casting)
{
    std::vector<RenderCommand> casters;
    casters.push_back(commandOf(scene.ground, scene.program, scene.material));
    casters.push_back(commandOf(scene.occluder, scene.program, scene.material));
    const auto caster_list = std::make_shared<const std::vector<RenderCommand>>(std::move(casters));

    std::vector<RenderCommand> receivers;
    receivers.push_back(commandOf(scene.ground, scene.program, scene.material));
    const auto receiver_list = std::make_shared<const std::vector<RenderCommand>>(std::move(receivers));

    const intrusive_ptr<Light> light = scene.light;
    light->setCastShadow(casting);
    std::vector<LightValue> lights{ light->value() };
    light->setCastShadow(true);

    FrameDescription frame;

    FrameDescription::Pass shadow_entry;
    shadow_entry.pass = scene.shadow_pass.get();
    shadow_entry.order = 0;
    PassDraw shadow_pass;
    shadow_pass.target = scene.map.get();
    shadow_pass.clear  = ClearPolicy{}; // clears the depth (the only attachment)
    shadow_pass.state.depth = DepthMode::TestAndWrite;
    shadow_pass.draws.push_back(sceneDraw(caster_list));
    shadow_entry.draw = std::move(shadow_pass);
    frame.passes.push_back(std::move(shadow_entry));

    FrameDescription::Pass main_entry;
    main_entry.pass   = scene.main_pass.get();
    main_entry.order  = 1;
    if (declare) {
        main_entry.inputs.push_back(ImageSource::producedTarget(scene.map.get()));
    }
    PassDraw main_pass;
    main_pass.target       = scene.target.get();
    main_pass.clear        = ClearPolicy{};
    main_pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    main_pass.state.depth  = DepthMode::TestAndWrite;
    main_pass.draws.push_back(sceneDraw(receiver_list, std::move(lights)));
    main_entry.draw = std::move(main_pass);
    frame.passes.push_back(std::move(main_entry));

    return frame;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback of the scene's colour target.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

/// @brief The luminance the shading wrote (green is enough: the material is white, so all three channels move together).
[[nodiscard]] int lumAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    return pixelAt(pixels, x, y)[1];
}

/// @brief Runs @p frame and reads the scene's colour attachment 0 back.
[[nodiscard]] std::vector<std::uint8_t> runAndRead(vn::graphics::RenderBackend& backend, const FrameDescription& frame, RenderTarget* target)
{
    (void)backend.executeFrame(frame, nullptr);
    std::vector<std::uint8_t> pixels;
    EXPECT_TRUE(backend.readColorBuffer(target, 0, pixels)) << "the scene's picture must read back";
    return pixels;
}

} // namespace

TEST(VkShadowTest, TheMapDarkensWhatTheCasterBlocks)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    Scene scene = makeScene();
    ASSERT_NE(scene.program, nullptr) << "the engine ships its forward program";

    // WHAT THE DRIVER SAYS DURING THE FRAME. The validation layer's messages arrive on this route, so they are the
    // only instrument that can judge the new shapes: the 0-colour render pass, the depth-only pipeline and the map's
    // layout transitions are all legal-or-not facts rather than picture facts.
    std::vector<vn::graphics::RenderDiagnostic> driver_messages;
    backend->setDiagnosticSink([&driver_messages](const vn::graphics::RenderDiagnostic& diagnostic) {
        driver_messages.push_back(diagnostic);
    });

    const std::vector<std::uint8_t> pixels = runAndRead(*backend, shadowFrame(scene, /*declare*/ true, /*casting*/ true), scene.target.get());

    // The left strip is behind the caster (z = 0 is in front of z = 0.5 along the light) and the right strip is not.
    const int shadowed = lumAt(pixels, 16, kTargetSize / 2);
    const int lit      = lumAt(pixels, 48, kTargetSize / 2);
    EXPECT_LT(shadowed, lit) << "the ground the caster covers is not darker than the ground beside it";
    EXPECT_GT(lit, 0) << "the unshadowed ground lost its light: the map's absence is not the answer";
    EXPECT_GT(shadowed, 0) << "the shadowed ground went black: the ambient term should survive a shadow";

    // The frame's shape: two pass scopes (the depth-only pass and the shading one) and TWO drawing calls - a drawing
    // call is the description a pass issues, not the commands inside it (the map's pass issues one call that draws its
    // two casters).
    ASSERT_TRUE(backend->frameReport().counters.has_value());
    EXPECT_EQ(backend->frameReport().counters->passes, 2U);
    EXPECT_EQ(backend->frameReport().counters->draws, 2U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U)
        << "the depth-only pipeline and the shadow ABI have to compile here";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    // NO ERROR FROM THE DRIVER: a validation ERROR here is an illegal barrier, layout or render pass - not a picture
    // that merely looks odd. A WARNING is expected and tolerated: the depth-only pass still draws with the program its
    // content declared, whose fragment stage writes a colour attachment this target does not have (the write is
    // discarded, and dropping the stage instead would change what a program that writes gl_FragDepth rasterises).
    for (const vn::graphics::RenderDiagnostic& message : driver_messages) {
        EXPECT_NE(message.severity, vn::graphics::DiagnosticSeverity::Error)
            << "the driver rejected a shadow-pass object: "
            << std::string(reinterpret_cast<const char*>(message.message.data()), message.message.size());
    }

    backend->shutdown();
}

TEST(VkShadowTest, WithoutACastingLightThePictureIsTheOneWithNoMapAtAll)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    Scene scene = makeScene();
    ASSERT_NE(scene.program, nullptr);

    // THE THREE FRAMES THAT MUST AGREE: the switch off with a map declared, the switch off with no map declared at all,
    // and the switch ON (which has to differ - otherwise "no difference" would prove nothing).
    const std::vector<std::uint8_t> switched_off  = runAndRead(*backend, shadowFrame(scene, /*declare*/ true, /*casting*/ false), scene.target.get());
    const std::vector<std::uint8_t> no_map        = runAndRead(*backend, shadowFrame(scene, /*declare*/ false, /*casting*/ false), scene.target.get());
    const std::vector<std::uint8_t> switched_on   = runAndRead(*backend, shadowFrame(scene, /*declare*/ true, /*casting*/ true), scene.target.get());

    ASSERT_EQ(switched_off.size(), no_map.size());
    EXPECT_EQ(switched_off, no_map)
        << "a map whose light stopped casting changed the picture: the switch is the LIGHT's, not the binding's";

    // A light switch off means EVERY pixel is lit the same way (same light, same normal): the two halves agree, and the
    // casting frame's covered half does not.
    EXPECT_EQ(lumAt(switched_off, 16, kTargetSize / 2), lumAt(switched_off, 48, kTargetSize / 2))
        << "the unshadowed frame shades two identical surfaces differently";
    EXPECT_LT(lumAt(switched_on, 16, kTargetSize / 2), lumAt(switched_on, 48, kTargetSize / 2))
        << "the casting frame lost its shadow: the case above would then prove nothing";

    backend->shutdown();
}

TEST(VkShadowTest, ADeclaredMapThatWasNeverDrawnShadesNothingInsteadOfReadingGarbage)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // THE MAP EXISTS, STATES ITS LIGHT, AND IS NEVER BUILT OR DRAWN: a frame that declares it must still shade the same
    // picture as a frame that declares nothing - the honest reading of "a map nobody produced is not a map".
    Scene scene = makeScene();
    ASSERT_NE(scene.program, nullptr);

    FrameDescription frame = shadowFrame(scene, /*declare*/ true, /*casting*/ true);
    // Drop the producing pass: the shading pass keeps its declaration, so the input resolves to a target this device
    // never built.
    frame.passes.erase(frame.passes.begin());
    const std::vector<std::uint8_t> unbuilt = runAndRead(*backend, frame, scene.target.get());

    Scene control = makeScene();
    const std::vector<std::uint8_t> none = runAndRead(*backend, shadowFrame(control, /*declare*/ false, /*casting*/ true), control.target.get());

    EXPECT_EQ(unbuilt, none) << "a declared map with no producer changed the picture: shading read an image nobody wrote";

    backend->shutdown();
}
