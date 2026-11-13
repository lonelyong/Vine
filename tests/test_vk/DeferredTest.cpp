/**
 * @brief M10: the engine's DEFERRED G-buffer frame, drawn by the hand-written Vulkan backend.
 *
 * WHAT THE TWO CASES PIN, and why each is a different question:
 *   - THE WHOLE FRAME: one scene pass writes the canonical four-attachment G-buffer (RGBA16F x4 + D24) through
 *     `ChainPresets::defaultGbufferGeometryProgram()`, and one SCREEN pass shades it through
 *     `ChainPresets::defaultDeferredLightProgram()` - sampling the G-buffer's four colour attachments at set 0
 *     bindings 0..3 and the light block at set 0 binding 6. The centre of the result must be LIT, which is the only
 *     thing that tells "the deferred chain ran" apart from "two targets were cleared".
 *   - EVERY ATTACHMENT WAS WRITTEN: the lighting program reads attachment 3's alpha as its write mask, so a G-buffer
 *     that only filled attachment 0 shades as background. The four copies below are the demo's own declarations -
 *     `screenCopyProgram(n)` over the same source target - and the assertions are on what they land. Attachment 2 is
 *     copied through its ALPHA channel (`ScreenCopyChannels::Alpha`), because that attachment's rgb is emission (zero
 *     for this material) while its alpha is the occlusion factor the G-buffer writer stores - the write evidence.
 *
 * THE SCENE IS STATED IN CLIP SPACE on purpose (identity camera, identity model): every number below is then the
 * value the hardware sees, with no camera derivation in the way. The triangle sits at clip z = 0.5 because the pass
 * policy TESTS depth and this backend is reverse-Z (the far plane is 0, which is what depth is cleared to).
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/String.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/ChainPresets.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ImageSource.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "PlanTestSupport.hpp"

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
using vn::graphics::LightType;
using vn::graphics::LightValue;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::ReadbackResult;
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
    camera.eye        = vn::math::Vec3d(0.0, 0.0, 0.0);
    return camera;
}

/// @brief A triangle in clip space at depth 0.5, with the view-space normal the G-buffer writer stores.
///
/// The z is 0.5 and not 0: the backend renders reverse-Z (the far plane is 0, which is what a depth buffer is cleared
/// to) and the default pass policy TESTS depth, so a fragment at the far plane would fail every comparison.
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle()
{
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -0.75F, -0.75F, 0.5F, // bottom left
        0.75F,  -0.75F, 0.5F, // bottom right
        0.0F,   0.75F,  0.5F, // top
    })));
    // The G-buffer's normal attachment carries the VIEW normal, so the geometry owes the vertex stage location 1.
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief The material the G-buffer projects: the engine's default surface, so the factors are the block's defaults.
[[nodiscard]] intrusive_ptr<Material> makeMaterial()
{
    auto material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf(0.8F, 0.8F, 0.8F, 1.0F));
    // The defaults state metallic 0, roughness 0.5 and occlusion 1 - the three values the lighting pass reads back out
    // of attachments 1 and 2, so they are stated here instead of being left to a second set of defaults.
    material->setMetallic(0.0F);
    material->setRoughness(0.5F);
    material->setOcclusion(1.0F);
    return material;
}

/// @brief The 64x64 RGBA8 + D24 target the lit picture (and each copy) is read from.
[[nodiscard]] intrusive_ptr<RenderTarget> makeColorTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_deferred_result"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief What both cases share: the targets, the drawable and the SDK's three programs.
struct Scene {
    intrusive_ptr<RenderTarget>  gbuffer;         // The canonical four-attachment G-buffer (ChainPresets).
    intrusive_ptr<RenderTarget>  result;          // The lit composite that is read back.
    intrusive_ptr<Geometry>      geometry;        // The one drawable.
    intrusive_ptr<Material>      material;        // Its surface.
    intrusive_ptr<ShaderProgram> gbuffer_program; // defaultGbufferGeometryProgram().
    intrusive_ptr<ShaderProgram> light_program;   // defaultDeferredLightProgram().
};

/// @brief Builds the scene.
[[nodiscard]] Scene makeScene()
{
    Scene scene;
    scene.gbuffer         = vn::graphics::defaultGbufferTarget(kTargetSize, kTargetSize);
    scene.result          = makeColorTarget();
    scene.geometry        = makeTriangle();
    scene.material        = makeMaterial();
    scene.gbuffer_program = vn::graphics::defaultGbufferGeometryProgram();
    scene.light_program   = vn::graphics::defaultDeferredLightProgram();
    return scene;
}

/// @brief One scene drawing call: the triangle through the G-buffer program, shaded with the scene's material.
[[nodiscard]] DrawDescription gbufferDraw(const Scene& scene)
{
    RenderCommand command(scene.geometry, nullptr, vn::math::Mat4d{});
    command.program              = scene.gbuffer_program;
    command.program_description  = scene.gbuffer_program->description();
    command.material             = scene.material;
    command.material_description = scene.material->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = std::move(commands);
    return draw;
}

/// @brief The G-buffer pass: the scene draw into the canonical target, every attachment cleared.
///
/// The clear states attachment 0's colour only and leaves `extra_colors` EMPTY on purpose: attachments 1..3 are then
/// cleared to transparent black, which is the G-buffer's own "nothing was drawn here" (attachment 3's alpha is the
/// lighting program's write mask, so the default is part of the contract rather than an omission).
[[nodiscard]] FrameDescription::Pass gbufferEntry(const Scene& scene)
{
    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target        = scene.gbuffer.get();
    pass.clear         = ClearPolicy{};
    pass.clear->color  = Colorf(0.0F, 0.0F, 0.0F, 0.0F);
    pass.clear->depth  = true;
    pass.state.depth   = DepthMode::TestAndWrite;
    pass.draws.push_back(gbufferDraw(scene));

    entry.draw = std::move(pass);
    return entry;
}

/// @brief The one ambient light the lighting call announces (the analytic fill the deferred program multiplies by).
[[nodiscard]] LightValue ambientLight()
{
    LightValue light;
    light.enabled   = true;
    light.type      = LightType::Ambient;
    light.color     = Colorf(1.0F, 1.0F, 1.0F, 1.0F);
    light.intensity = 1.0F;
    return light;
}

/// @brief The frame: the G-buffer pass, then the deferred lighting pass that samples it.
///
/// The lighting pass DECLARES the G-buffer as an input (`FrameDescription::Pass::inputs`) and names it as the screen
/// draw's source - the two halves of one statement, and the one this backend reads the sampled attachments from.
[[nodiscard]] FrameDescription deferredFrame(const Scene& scene)
{
    FrameDescription frame;
    frame.passes.push_back(gbufferEntry(scene));

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 1;
    entry.inputs.push_back(ImageSource::producedTarget(scene.gbuffer.get()));

    PassDraw pass;
    pass.target       = scene.result.get();
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    // The destination carries depth, and a full-screen triangle sits at the far plane: the pass states that it draws
    // OVER what the target holds, which is the same policy the SDK's own lighting pass states by having no depth.
    pass.state.depth = DepthMode::Disabled;

    DrawDescription draw;
    draw.kind                 = DrawDescription::Kind::Screen;
    draw.camera               = identityCamera();
    draw.source               = scene.gbuffer.get();
    draw.program              = scene.light_program;
    draw.program_description  = scene.light_program->description();
    draw.light_values         = std::vector<LightValue>{ ambientLight() };
    pass.draws.push_back(std::move(draw));

    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief A frame with ONE pass that clears @p destination and copies one attachment of @p source into it.
[[nodiscard]] FrameDescription copyFrame(RenderTarget* source, RenderTarget* destination, const intrusive_ptr<ShaderProgram>& program)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;
    entry.inputs.push_back(ImageSource::producedTarget(source));

    PassDraw pass;
    pass.target       = destination;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.state.depth  = DepthMode::Disabled;

    DrawDescription draw;
    draw.kind                = DrawDescription::Kind::Screen;
    draw.camera              = identityCamera();
    draw.source              = source;
    draw.program             = program;
    draw.program_description = program->description();
    pass.draws.push_back(std::move(draw));

    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

} // namespace

TEST(VkDeferredTest, TheDeferredFrameLightsTheGbuffer)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    Scene scene = makeScene();
    ASSERT_NE(scene.gbuffer_program, nullptr) << "the SDK ships the G-buffer geometry program";
    ASSERT_NE(scene.light_program, nullptr) << "the SDK ships the deferred lighting program";
    ASSERT_EQ(scene.gbuffer->colorCount(), 4) << "the canonical G-buffer is four colour attachments (see defaultGbufferTarget)";

    // WHAT THE DRIVER SAYS DURING THE FRAME: the MRT pass, the four RGBA16F attachments and the source transition the
    // screen draw needs are legal-or-not facts rather than picture facts, and this is the only instrument for them.
    std::vector<vn::graphics::RenderDiagnostic> driver_messages;
    backend->setDiagnosticSink([&driver_messages](const vn::graphics::RenderDiagnostic& diagnostic) {
        driver_messages.push_back(diagnostic);
    });

    const PresentOutcome outcome = backend->executeFrame(deferredFrame(scene), nullptr);
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "a device with no window presents nothing; a target is not a surface";

    std::vector<std::uint8_t> pixels;
    ReadbackResult            why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(scene.result.get(), 0, pixels, &why)) << "readback refused: " << static_cast<int>(why);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

    const std::array<int, 4> centre = pixelAt(pixels, kTargetSize / 2, kTargetSize / 2);
    const std::array<int, 4> clear{ 0, 0, 0, 255 };
    std::printf("[vk-deferred] result centre r=%d g=%d b=%d a=%d (clear %d,%d,%d,%d)\n",
                centre[0],
                centre[1],
                centre[2],
                centre[3],
                clear[0],
                clear[1],
                clear[2],
                clear[3]);

    // THE PIXEL INSIDE THE TRIANGLE IS THE LIT SURFACE: the G-buffer stored the material and the lighting pass
    // multiplied it by the ambient term, so a zero here means the screen draw never read the G-buffer.
    EXPECT_GT(centre[0] + centre[1] + centre[2], 0)
        << "the centre of the lit composite is black: the deferred lighting pass did not shade the G-buffer";
    EXPECT_NE(centre, clear) << "the lit centre is the clear colour, so the picture is indistinguishable from an empty target";
    EXPECT_EQ(centre[3], 255) << "the lighting pass writes an opaque composite";

    // THE FRAME'S SHAPE: two pass scopes, and one drawing call each (the G-buffer pass and the lighting pass).
    ASSERT_TRUE(backend->frameReport().counters.has_value());
    EXPECT_EQ(backend->frameReport().counters->passes, 2U);
    EXPECT_EQ(backend->frameReport().counters->draws, 2U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U)
        << "the SDK's G-buffer and deferred lighting programs have to compile on this backend";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "a drawing call of the deferred chain was skipped";
    for (const vn::graphics::RenderDiagnostic& message : driver_messages) {
        EXPECT_NE(message.severity, vn::graphics::DiagnosticSeverity::Error)
            << "the driver rejected an object of the deferred frame: "
            << std::string(reinterpret_cast<const char*>(message.message.data()), message.message.size());
    }

    backend->shutdown();
}

TEST(VkDeferredTest, EveryGbufferAttachmentWasWritten)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    Scene scene = makeScene();
    ASSERT_NE(scene.gbuffer_program, nullptr);
    ASSERT_EQ(scene.gbuffer->colorCount(), 4);

    // THE G-BUFFER, ONCE: a frame of its own so the four copies below sample what ONE geometry pass wrote.
    FrameDescription gbuffer_frame;
    gbuffer_frame.passes.push_back(gbufferEntry(scene));
    (void)backend->executeFrame(gbuffer_frame, nullptr);

    std::array<std::array<int, 4>, 4> copies{};
    for (int attachment = 0; attachment < 4; ++attachment) {
        const intrusive_ptr<RenderTarget> destination = makeColorTarget();
        // Attachment 2 is copied through its ALPHA: its rgb is emission (zero for this material), its alpha is the
        // occlusion factor the G-buffer writer stores, so the alpha is that row's write evidence.
        const intrusive_ptr<ShaderProgram> copy = vn::graphics::screenCopyProgram(
            attachment,
            attachment == 2 ? vn::graphics::ScreenCopyChannels::Alpha : vn::graphics::ScreenCopyChannels::All);
        ASSERT_NE(copy, nullptr) << "the SDK ships the screen copy program";

        (void)backend->executeFrame(copyFrame(scene.gbuffer.get(), destination.get(), copy), nullptr);

        std::vector<std::uint8_t> pixels;
        ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels))
            << "the copy of attachment " << attachment << " must read back";
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

        copies[static_cast<std::size_t>(attachment)] = pixelAt(pixels, kTargetSize / 2, kTargetSize / 2);
        std::printf("[vk-deferred] gbuffer attachment %d copy: r=%d g=%d b=%d a=%d\n",
                    attachment,
                    copies[static_cast<std::size_t>(attachment)][0],
                    copies[static_cast<std::size_t>(attachment)][1],
                    copies[static_cast<std::size_t>(attachment)][2],
                    copies[static_cast<std::size_t>(attachment)][3]);
    }

    for (int attachment = 0; attachment < 4; ++attachment) {
        const std::array<int, 4>& pixel = copies[static_cast<std::size_t>(attachment)];
        // Attachment 2's copy carries the occlusion factor in ALL three channels (the alpha variant writes vec3(a)),
        // so "written" is a red channel above the midpoint; the other three carry their stored data directly.
        const bool written = attachment == 2 ? pixel[0] > 128
                                             : (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 0);
        EXPECT_TRUE(written) << "attachment " << attachment
                             << " copied its clear value: the geometry pass did not write that G-buffer attachment";
    }
    // The attachment INDEX really moved something: a binding pinned at 0 would make every copy the base colour.
    EXPECT_NE(copies[1], copies[0])
        << "the normal attachment and the base-colour attachment read the same pixel: the copy did not follow the index";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U)
        << "the four screen copy programs have to compile on this backend";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "a copy of a G-buffer attachment was skipped";

    backend->shutdown();
}
