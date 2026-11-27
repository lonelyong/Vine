/**
 * @brief M9: a SHADOW MAP - a depth-only pass this backend records, and the engine's own forward program sampling it.
 *
 * WHY THIS IS THE HARDEST SHAPE THE BACKEND HAS HAD. Three facts have to agree, and each of them is somebody else's
 * statement:
 *   * the TARGET says it is a map (`RenderTarget::setShadowOf`) and how to read it (`setProducerViewProjection`), and it
 *     has NO colour attachment - so the pass that writes it is a depth-only render pass with a depth-only pipeline;
 *   * the CONSUMING pass declares the map as an input, and this backend has to move the map's depth image out of the
 *     attachment layout (an image barrier is not legal inside a render pass, so it happens before the pass begins);
 *   * the LIGHT the map belongs to must still be a caster among the lights the draw packet announced: `VineShadowBlock`
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
#include <algorithm>
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
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/DepthPolicy.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ImageInput.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/PassSelection.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>

using vn::BufferSlice;
using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AlphaMode;
using vn::graphics::AttachmentPolicy;
using vn::graphics::CameraSnapshot;
using vn::graphics::ColorReadback;
using vn::graphics::DepthPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::ImageSource;
using vn::graphics::Light;
using vn::graphics::LightValue;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PassSelection;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;

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
///
/// @note The quads below face +z and wind counter-clockwise in the picture, which the SDK calls FRONT. That pairing was
///       briefly "fixed" the other way while the backend's front-face constant was inverted (review 17); the constant is
///       measured now (`VkContentDrawTest.TheFragmentStageSeesTheSdksFrontFace`), so this fixture is back to the content
///       a well-formed author would write.
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
[[nodiscard]] DrawItem commandOf(const intrusive_ptr<Geometry>& geometry,
                                      const intrusive_ptr<ShaderProgram>& program,
                                      const intrusive_ptr<Material>& material)
{
    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    return command;
}

/// @brief One scene draw packet: @p commands through the identity camera, optionally announcing @p lights.
[[nodiscard]] DrawPacket sceneDraw(std::shared_ptr<const std::vector<DrawItem>> commands,
                                        vn::graphics::LightSetPtr                         lights = nullptr)
{
    DrawPacket draw;
    draw.kind        = DrawKind::Scene;
    draw.view.camera      = identityCamera();
    draw.commands    = std::move(commands);
    draw.light_values = std::move(lights);
    return draw;
}

/// @brief A full-screen quad at depth @p z that CARRIES UVs: a Mask material's silhouette is read through them.
[[nodiscard]] intrusive_ptr<Geometry> makeUvQuad(float z)
{
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
      -1.0F,
      -1.0F,
      z,
      1.0F,
      -1.0F,
      z,
      1.0F,
      1.0F,
      z,
      -1.0F,
      -1.0F,
      z,
      1.0F,
      1.0F,
      z,
      -1.0F,
      1.0F,
      z,
    })));
    // The engine's forward vertex stage declares normals unconditionally, so a geometry without them draws undefined
    // values (and validation says so).
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
      0.0F,
      0.0F,
      1.0F,
      0.0F,
      0.0F,
      1.0F,
      0.0F,
      0.0F,
      1.0F,
      0.0F,
      0.0F,
      1.0F,
      0.0F,
      0.0F,
      1.0F,
      0.0F,
      0.0F,
      1.0F,
    })));
    // One UV set spanning the quad left to right, so u < 0.5 is the quad's left half.
    geometry->setTexcoords2(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
      0.0F,
      0.0F,
      1.0F,
      0.0F,
      1.0F,
      1.0F,
      0.0F,
      0.0F,
      1.0F,
      1.0F,
      0.0F,
      1.0F,
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief A 4x1 base-colour map whose ALPHA is opaque on the left half and clear on the right (a cut-out's shape).
[[nodiscard]] intrusive_ptr<Texture2D> makeCutOutMap()
{
    auto                       image  = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(4, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texels = image->mipData(0);
    for (int texel = 0; texel < 4; ++texel) {
        const std::size_t at = static_cast<std::size_t>(texel) * 4U;
        texels[at + 0U]      = std::byte{ 255U };
        texels[at + 1U]      = std::byte{ 255U };
        texels[at + 2U]      = std::byte{ 255U };
        texels[at + 3U]      = texel < 2 ? std::byte{ 255U } : std::byte{ 0U };
    }
    auto map = intrusive_ptr<Texture2D>(new Texture2D(4, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    map->setImage(image);
    return map;
}

/// @brief A depth-only map whose DEPTH CAN BE READ BACK.
///
/// @note D24 is combined depth/stencil - the backend refuses to copy it as plain depth (see
///       `VkContentTargetTest`), so a case that judges the map by its texels states a format it can read.
[[nodiscard]] intrusive_ptr<RenderTarget> makeReadableMap()
{
    auto map = intrusive_ptr<RenderTarget>(new RenderTarget());
    map->setName(String(u8"vk_caster_map"));
    map->attachDepth(RenderTarget::DepthFormat::D32F);
    map->setSize(kTargetSize, kTargetSize);
    return map;
}

/// @brief One DEPTH-ONLY pass: @p commands into @p map, through the engine's depth program, under @p selection.
[[nodiscard]] FrameDescription depthFrame(vn::graphics::RenderPass* pass, RenderTarget* map, std::vector<DrawItem> commands, PassSelection selection)
{
    FrameDescription       frame;
    FrameDescription::Pass entry;
    entry.pass  = pass;
    entry.order = 0;

    const intrusive_ptr<ShaderProgram> depth_program = vn::graphics::depthOnlyProgram();
    PassDraw                           draw;
    draw.target                       = map;
    draw.attachment_policy            = AttachmentPolicy{}; // clears the depth (the only attachment)
    draw.state.depth                  = DepthPolicy::TestAndWrite;
    draw.selection                    = selection;
    // THE PASS OVERRIDE the chain states too (see ChainBuilder::buildShadowPass): a depth map has no colour
    // attachment, so the content's own program is not what this pass draws with.
    draw.program_override             = depth_program;
    draw.program_override_description = depth_program->description();
    draw.packets.push_back(sceneDraw(std::make_shared<const std::vector<DrawItem>>(std::move(commands))));
    entry.draw = std::move(draw);
    frame.passes.push_back(std::move(entry));
    return frame;
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
    std::vector<DrawItem> casters;
    casters.push_back(commandOf(scene.ground, scene.program, scene.material));
    casters.push_back(commandOf(scene.occluder, scene.program, scene.material));
    const auto caster_list = std::make_shared<const std::vector<DrawItem>>(std::move(casters));

    std::vector<DrawItem> receivers;
    receivers.push_back(commandOf(scene.ground, scene.program, scene.material));
    const auto receiver_list = std::make_shared<const std::vector<DrawItem>>(std::move(receivers));

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
    shadow_pass.attachment_policy = AttachmentPolicy{}; // clears the depth (the only attachment)
    shadow_pass.state.depth = DepthPolicy::TestAndWrite;
    shadow_pass.packets.push_back(sceneDraw(caster_list));
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
    main_pass.attachment_policy        = AttachmentPolicy{};
    main_pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    main_pass.state.depth  = DepthPolicy::TestAndWrite;
    main_pass.packets.push_back(sceneDraw(receiver_list, vn::graphics::LightSet::create(std::move(lights))));
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
    const ColorReadback read = backend.readColorImage(target, 0);
    EXPECT_TRUE(read.ok()) << "the scene's picture must read back";
    return read.values;
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

    // The frame's shape: two pass scopes (the depth-only pass and the shading one) and TWO draw packets - a drawing
    // call is the description a pass issues, not the commands inside it (the map's pass issues one call that draws its
    // two casters).
    ASSERT_TRUE(backend->frameReport().counters.has_value());
    EXPECT_EQ(backend->frameReport().counters->passes, 2U);
    EXPECT_EQ(backend->frameReport().counters->draw_packets, 2U);
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

TEST(VkShadowTest, AFullscreenPassShadesWithTheMapItResolved)
{
    // THE FULL-SCREEN HALF OF THE SAME ABI (2026-11-14): the engine's shadowed deferred-lighting program is a SCREEN
    // pass, and a full-screen pass is where the sun's shadow has to arrive. The set's binding 7 must therefore carry the
    // map the pass RESOLVED - not the white stand-in the full-screen set offers when there is none. The app found
    // this the hard way: the deferred demo had no shadow on vk while the content path's shadow worked.
    //
    // The program below samples the map directly and writes its depth, so "which image arrived at binding 7" is a
    // number: the ground's depth over the centre, the caster's over the strip it covers, and 1.0 (the stand-in's
    // white) if the real map never got bound.
    constexpr const char8_t* kFullscreenVertex = u8R"(
#version 450
void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";
    constexpr const char8_t* kMapReadFragment = u8R"(
#version 450
layout(location = 0) in vec2 vn_uv;
layout(binding = 7) uniform sampler2D vn_shadow_map;
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(vec3(texture(vn_shadow_map, vn_uv).r), 1.0);
}
)";

    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    Scene scene = makeScene();
    ASSERT_NE(scene.program, nullptr);

    auto read_program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    read_program->setName(u8"vk_shadow_read");
    read_program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Vertex, String(kFullscreenVertex), String(u8"main") });
    read_program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Fragment, String(kMapReadFragment), String(u8"main") });

    // The picture: casters into the map (the same first pass every case here builds), then a source target that only
    // has to EXIST (the full-screen ABI needs a source; this program does not sample it), then the full-screen pass itself.
    const intrusive_ptr<RenderTarget> destination = makeColorTarget();
    const intrusive_ptr<RenderTarget> source      = makeColorTarget();

    // The map's content is stated directly, so the sampled numbers are unambiguous: a strip at z = 0.5 covers the
    // LEFT half and the clear leaves the right half at 0. Reverse-Z's GREATER keeps the strip's larger depth.
    const intrusive_ptr<Geometry> strip = makeQuad(-1.0F, 0.0F, 0.5F);
    std::vector<DrawItem>    casters;
    casters.push_back(commandOf(strip, scene.program, scene.material));
    const auto caster_list = std::make_shared<const std::vector<DrawItem>>(std::move(casters));

    FrameDescription frame;

    FrameDescription::Pass shadow_entry;
    shadow_entry.pass = scene.shadow_pass.get();
    shadow_entry.order = 0;
    PassDraw shadow_pass;
    shadow_pass.target      = scene.map.get();
    shadow_pass.attachment_policy = AttachmentPolicy{};
    shadow_pass.state.depth = DepthPolicy::TestAndWrite;
    shadow_pass.packets.push_back(sceneDraw(caster_list));
    shadow_entry.draw = std::move(shadow_pass);
    frame.passes.push_back(std::move(shadow_entry));

    // A pass identity of its own: two entries naming ONE pass would describe a frame the engine never
    // produces (it announces one entry per registered pass), and a case should not rely on the backend
    // tolerating a shape the frame value's contract does not allow.
    const intrusive_ptr<vn::graphics::RenderPass> source_pass_identity(new vn::graphics::RenderPass());
    FrameDescription::Pass source_entry;
    source_entry.pass  = source_pass_identity.get();
    source_entry.order = 1;
    PassDraw source_pass;
    source_pass.target      = source.get();
    source_pass.attachment_policy = AttachmentPolicy{};
    source_pass.state.depth = DepthPolicy::Disabled;
    source_entry.draw       = std::move(source_pass);
    frame.passes.push_back(std::move(source_entry));

    DrawPacket fullscreen_draw;
    fullscreen_draw.kind                = DrawKind::Fullscreen;
    fullscreen_draw.view.camera              = identityCamera();
    fullscreen_draw.source              = source.get();
    fullscreen_draw.program             = read_program;
    fullscreen_draw.program_description = read_program->description();

    const intrusive_ptr<vn::graphics::RenderPass> fullscreen_pass_identity(new vn::graphics::RenderPass());
    FrameDescription::Pass                        fullscreen_entry;
    fullscreen_entry.pass  = fullscreen_pass_identity.get();
    fullscreen_entry.order = 2;
    fullscreen_entry.inputs.push_back(ImageSource::producedTarget(scene.map.get()));
    PassDraw fullscreen_pass;
    fullscreen_pass.target = destination.get();
    fullscreen_pass.packets.push_back(std::move(fullscreen_draw));
    fullscreen_entry.draw = std::move(fullscreen_pass);
    frame.passes.push_back(std::move(fullscreen_entry));

    const std::vector<std::uint8_t> pixels = runAndRead(*backend, frame, destination.get());

    // The strip's own depth over the left half, the clear's (0) over the right. The number is the DEVICE depth the
    // content stage wrote: the stated clip z = 0.5 goes through the backend's reverse-Z camera fold (0.5 - 0.5 * z),
    // so the map holds 0.25 - and 0.25 * 255 = 64. A white stand-in would read 255 in both halves, and a map that
    // never reached binding 7 could not produce two different values.
    EXPECT_NEAR(lumAt(pixels, 8, kTargetSize / 2), 64, 2)
        << "the full-screen pass did not sample the resolved map's strip depth";
    EXPECT_LE(lumAt(pixels, 48, kTargetSize / 2), 4)
        << "the full-screen pass did not sample the map's cleared half";

    ASSERT_TRUE(backend->frameReport().counters.has_value());
    EXPECT_EQ(backend->frameReport().counters->draw_packets, 2U) << "the map's pass and the full-screen pass both drew";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "the full-screen pass was refused instead of shaded";

    backend->shutdown();
}

TEST(VkShadowTest, AMultisampledMapCastsTheSameShadow)
{
    // BACKLOG T40: a multisampled map RESOLVES its depth, and the shading pass samples that resolved picture. The claim
    // is byte-equality with the same scene's single-sampled map: the mode is `sample0`, so the picture a later pass
    // reads is the depth a single-sample draw would have written - and a resolve that never ran, or a sampling pass
    // that still bound the multisampled attachment, would shade a different (or garbage) picture.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    const vn::graphics::Capabilities capabilities = backend->capabilities();
    if (capabilities.limits.max_samples <= 1U) {
        GTEST_SKIP() << "this device genuinely reports no multisampling (Capabilities::max_samples is 1)";
    }

    std::vector<vn::graphics::RenderDiagnostic> driver_messages;
    backend->setDiagnosticSink([&driver_messages](const vn::graphics::RenderDiagnostic& diagnostic) { driver_messages.push_back(diagnostic); });
    const std::size_t reported_before = backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest);

    Scene sharp = makeScene();
    ASSERT_NE(sharp.program, nullptr);
    const std::vector<std::uint8_t> single_sampled_map = runAndRead(*backend, shadowFrame(sharp, /*declare*/ true, /*casting*/ true), sharp.target.get());

    Scene smooth = makeScene();
    ASSERT_NE(smooth.program, nullptr);
    // The SDK default already lets a shader sample the map's depth; the sample count is the one thing that changes.
    ASSERT_TRUE(smooth.map->isDepthPromoted());
    smooth.map->setSamples(std::min<std::uint32_t>(4U, capabilities.limits.max_samples));
    const std::vector<std::uint8_t> multisampled_map = runAndRead(*backend, shadowFrame(smooth, /*declare*/ true, /*casting*/ true), smooth.target.get());

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest), reported_before)
        << "a multisampled map is served now (see TargetObjects::resolvesDepth)";
    ASSERT_EQ(multisampled_map.size(), single_sampled_map.size());
    EXPECT_EQ(multisampled_map, single_sampled_map)
        << "the multisampled map shaded a different picture: the depth resolve or the sampling of its picture is wrong";
    // The picture must still BE the shadowed one - equal-but-wrong would pass the comparison above if neither frame
    // shaded at all.
    EXPECT_LT(lumAt(multisampled_map, 16, kTargetSize / 2), lumAt(multisampled_map, 48, kTargetSize / 2))
        << "the multisampled map's frame lost its shadow";
    ASSERT_TRUE(backend->frameReport().counters.has_value());
    EXPECT_EQ(backend->frameReport().counters->draw_packets, 2U) << "the map's pass and the shading pass both ran";
    for (const vn::graphics::RenderDiagnostic& message : driver_messages) {
        EXPECT_NE(message.severity, vn::graphics::DiagnosticSeverity::Error)
            << "the driver rejected an object of the multisampled map's frame: "
            << std::string(reinterpret_cast<const char*>(message.message.data()), message.message.size());
    }

    backend->shutdown();
}

TEST(VkShadowTest, ASingleSampledMapDrawnAgainStatesItsDepthAgain)
{
    // THE OTHER SHAPE OF THE SAME FACT. A single-sampled map's depth image is its ATTACHMENT and its PICTURE at once:
    // the shading pass samples it (SHADER_READ_ONLY) and the next frame's map pass attaches it again, so the barrier
    // back to the depth attachment layout is the attachment's own. A recorder that states the picture's layout only
    // for a RESOLVE image - which does not exist here - leaves that barrier out, and validation names
    // `VUID-vkCmdBeginRendering-pRenderingInfo-09588` at the second frame's submit.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    std::vector<vn::graphics::RenderDiagnostic> driver_messages;
    backend->setDiagnosticSink([&driver_messages](const vn::graphics::RenderDiagnostic& diagnostic) { driver_messages.push_back(diagnostic); });

    Scene scene = makeScene();
    ASSERT_NE(scene.program, nullptr);

    const std::vector<std::uint8_t> first = runAndRead(*backend, shadowFrame(scene, /*declare*/ true, /*casting*/ true), scene.target.get());
    const std::vector<std::uint8_t> second = runAndRead(*backend, shadowFrame(scene, /*declare*/ true, /*casting*/ true), scene.target.get());

    EXPECT_EQ(second, first) << "the second frame's map is not the picture the first one resolved";
    EXPECT_LT(lumAt(second, 16, kTargetSize / 2), lumAt(second, 48, kTargetSize / 2)) << "the second frame lost its shadow";
    for (const vn::graphics::RenderDiagnostic& message : driver_messages) {
        EXPECT_NE(message.severity, vn::graphics::DiagnosticSeverity::Error)
            << "the driver rejected an object of the map's second frame: "
            << std::string(reinterpret_cast<const char*>(message.message.data()), message.message.size());
    }

    backend->shutdown();
}

TEST(VkShadowTest, AMultisampledMapDrawnAgainStatesBothDepthImages)
{
    // TWO FRAMES, ONE MAP. The first frame writes the map's multisampled depth and then SAMPLES its resolved picture, so
    // the second frame is the first one that has to state where BOTH images are: the SAMPLE BUFFER (still a depth
    // attachment) and the PICTURE (SHADER_READ_ONLY since it was sampled). They are two images with two layouts, and a
    // recorder that derives one from the other names an illegal `oldLayout` for one of them: validation reports it,
    // and a driver that honours the value discards a picture that was meant to be kept.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    const vn::graphics::Capabilities capabilities = backend->capabilities();
    if (capabilities.limits.max_samples <= 1U) {
        GTEST_SKIP() << "this device genuinely reports no multisampling (Capabilities::max_samples is 1)";
    }

    std::vector<vn::graphics::RenderDiagnostic> driver_messages;
    backend->setDiagnosticSink([&driver_messages](const vn::graphics::RenderDiagnostic& diagnostic) { driver_messages.push_back(diagnostic); });

    Scene scene = makeScene();
    ASSERT_NE(scene.program, nullptr);
    ASSERT_TRUE(scene.map->isDepthPromoted());
    scene.map->setSamples(std::min<std::uint32_t>(4U, capabilities.limits.max_samples));

    const std::vector<std::uint8_t> first = runAndRead(*backend, shadowFrame(scene, /*declare*/ true, /*casting*/ true), scene.target.get());
    // THE MAP'S DEPTH PICTURE IS SHADER_READ_ONLY NOW: the next pass into the map must move IT back, and must not claim
    // the same about the sample buffer, which nothing has ever sampled.
    const std::vector<std::uint8_t> second = runAndRead(*backend, shadowFrame(scene, /*declare*/ true, /*casting*/ true), scene.target.get());

    EXPECT_EQ(second, first) << "the second frame's map resolved a different picture than the first";
    EXPECT_LT(lumAt(second, 16, kTargetSize / 2), lumAt(second, 48, kTargetSize / 2)) << "the second frame lost its shadow";
    for (const vn::graphics::RenderDiagnostic& message : driver_messages) {
        EXPECT_NE(message.severity, vn::graphics::DiagnosticSeverity::Error)
            << "the driver rejected an object of the map's second frame: "
            << std::string(reinterpret_cast<const char*>(message.message.data()), message.message.size());
    }

    backend->shutdown();
}

/**
 * @brief A MASK caster's silhouette is what the map holds (review 18 Y7's cut-out half).
 *
 * THE FAILURE THIS PINS. The depth stage used to declare no output and read nothing, so every drawable of a depth pass
 * wrote its whole quad: a cut-out leaf cast the shadow of its rectangle. One depth texel can describe a surface whose
 * coverage is all-or-nothing, so the map has to reproduce the MATERIAL's threshold - the same call the colour pass
 * makes (`vine_alpha.glsl`), not a second copy of it.
 *
 * WHY THE DEPTH IS READ AND NOT THE PICTURE. "The shadow is leaf-shaped" is a claim about the map, and the map is a
 * value this device can hand back: the covered half carries the caster's depth (clip z = 0 folds to 0.5), the cut-away
 * half still carries the pass' CLEAR (the reverse-Z far plane, 0).
 */
TEST(VkShadowTest, AMaskedCastersSilhouetteIsTheShapeTheMapHolds)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  map     = makeReadableMap();
    const intrusive_ptr<ShaderProgram> program = vn::graphics::depthOnlyProgram();
    ASSERT_NE(program, nullptr) << "the engine ships its depth-only program";
    const intrusive_ptr<Geometry>  caster  = makeUvQuad(0.0F);
    const intrusive_ptr<Texture2D> cut_out = makeCutOutMap();
    auto                           pass    = intrusive_ptr<vn::graphics::RenderPass>(new vn::graphics::RenderPass());

    // The caster: a Mask material whose base map's alpha is zero over the quad's right half.
    auto masked = intrusive_ptr<Material>(new Material());
    masked->setBaseColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    masked->setTexture(TextureSlot::BaseColor, cut_out);
    masked->setAlphaMode(AlphaMode::Mask);
    masked->setAlphaCutoff(0.5F);

    EXPECT_EQ(backend->executeFrame(depthFrame(pass.get(), map.get(), { commandOf(caster, program, masked) }, PassSelection::Opaque), nullptr),
              vn::graphics::PresentOutcome::Skipped);

    vn::graphics::DepthReadback read = backend->readDepthImage(map.get());
    ASSERT_TRUE(read.ok()) << "the map's depth must read back: " << static_cast<int>(read.status);
    ASSERT_EQ(read.values.size(), std::size_t{ kTargetSize * kTargetSize }) << "one value per texel";
    const auto depthAt = [&read](int x, int y) { return read.values[static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)]; };
    EXPECT_NEAR(depthAt(8, kTargetSize / 2), 0.5F, 0.002F) << "the opaque half of the cut-out lost the caster: the mask must not cut what its map keeps";
    EXPECT_FLOAT_EQ(depthAt(56, kTargetSize / 2), 0.0F) << "the cut-away half still holds a distance: the map's shape is the quad's, not the leaf's";

    // THE CONTROL: the SAME map, the SAME quad, an OPAQUE material (its alpha is ignored). Both halves are written, so
    // the hole above is the alpha MODE's rule and not a draw that never happened.
    auto opaque = intrusive_ptr<Material>(new Material());
    opaque->setBaseColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    opaque->setTexture(TextureSlot::BaseColor, cut_out);

    EXPECT_EQ(backend->executeFrame(depthFrame(pass.get(), map.get(), { commandOf(caster, program, opaque) }, PassSelection::Opaque), nullptr),
              vn::graphics::PresentOutcome::Skipped);
    read = backend->readDepthImage(map.get());
    ASSERT_TRUE(read.ok());
    EXPECT_NEAR(depthAt(56, kTargetSize / 2), 0.5F, 0.002F)
        << "an Opaque material's map alpha still cut the map: the cut-out is the Mask mode's rule, not the map's value";

    backend->shutdown();
}

/**
 * @brief A depth pass records what OCCLUDES, and a blended surface does not (review 18 Y7's policy half).
 *
 * ONE DISTANCE CANNOT DESCRIBE PARTIAL COVERAGE. A blended drawable's colours are a mix with what is behind it, so a
 * depth texel written for it would shadow everything under the parts its colour only tints - the map would be darker
 * than the picture it belongs to. The pipeline states that as `PassSelection` (a depth pass draws the sort's OPAQUE
 * queue), and the pass' own list stays the shared one the colour passes draw: the backend applies the selection per
 * command, the same way it applies the pass' state and program override.
 *
 * The same frame is run twice: once as a depth pass states it, once with EVERY command selected. The map's texels are
 * the answer, so "nothing was recorded" cannot be confused with "the device failed".
 */
TEST(VkShadowTest, ADepthPassRecordsOnlyWhatCanOcclude)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  map     = makeReadableMap();
    const intrusive_ptr<ShaderProgram> program = vn::graphics::depthOnlyProgram();
    ASSERT_NE(program, nullptr);
    const intrusive_ptr<Geometry> caster = makeUvQuad(0.0F);
    auto                          pass   = intrusive_ptr<vn::graphics::RenderPass>(new vn::graphics::RenderPass());

    auto blended = intrusive_ptr<Material>(new Material());
    blended->setBaseColor(Colorf(1.0F, 1.0F, 1.0F, 0.25F));
    blended->setAlphaMode(AlphaMode::Blend);
    ASSERT_TRUE(commandOf(caster, program, blended).isTransparent()) << "the fixture must be a transparent command";

    EXPECT_EQ(backend->executeFrame(depthFrame(pass.get(), map.get(), { commandOf(caster, program, blended) }, PassSelection::Opaque), nullptr),
              vn::graphics::PresentOutcome::Skipped);
    vn::graphics::DepthReadback read = backend->readDepthImage(map.get());
    ASSERT_TRUE(read.ok()) << "the map's depth must read back: " << static_cast<int>(read.status);
    for (const float value : read.values) {
        EXPECT_FLOAT_EQ(value, 0.0F) << "a blended drawable wrote a distance into the map: it would shadow what it only tints";
    }

    // THE SAME FRAME WITH NOTHING EXCLUDED: the very command the depth pass skipped is recorded, so the emptiness above
    // is the selection's answer and not a program, a pipeline or a stream that could not be built.
    EXPECT_EQ(backend->executeFrame(depthFrame(pass.get(), map.get(), { commandOf(caster, program, blended) }, PassSelection::All), nullptr),
              vn::graphics::PresentOutcome::Skipped);
    read = backend->readDepthImage(map.get());
    ASSERT_TRUE(read.ok());
    EXPECT_NEAR(read.values[static_cast<std::size_t>(kTargetSize / 2) * kTargetSize + kTargetSize / 2], 0.5F, 0.002F)
        << "with every command selected the caster must reach the map: the case above would then prove nothing";

    backend->shutdown();
}
