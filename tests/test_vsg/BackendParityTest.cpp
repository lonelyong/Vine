/**
 * @brief M7: ONE FrameDescription, drawn by both backends - the same record and the same picture.
 *
 * WHY THIS FILE LIVES IN THE REFERENCE BACKEND'S SUITE. The parity claim is "the seam is enough": a frame value
 * that the reference backend draws must be drawn by a second, independently written backend with the same record
 * and the same pixels (`.ai/design/vk-backend.md` 2.10 states the claim and names this suite as its evidence). The
 * reference half is the vsg device this suite already brings up for its pixel cases; the vk half is the same
 * backend `test_vk` exercises, compiled into this binary. Two backends in ONE PROCESS is the point - a diff
 * between two processes would compare files, not answers.
 *
 * WHAT IS COMPARED, AND WHAT IS NOT.
 * - THE RECORD: `FrameReport::counters` (pass scopes, draw packets) and `PresentOutcome`, for the same call.
 *   Both are the SDK's own vocabulary for "what shape was this frame", so the two numbers are comparable by
 *   contract rather than by convention - and where they disagreed, the frame value means two things.
 * - THE PICTURE: `readColorBuffer` of the same target, BYTE for byte. The cases are chosen so that a correct
 *   backend has no freedom in the result (single sample, a 1x1 sample or a full-screen copy, one documented
 *   blend), because a pixel diff with an unexplained tolerance would hide exactly the bug it exists to find
 *   (`G10`).
 * - NOT COMPARED: frame uploads, residency and depth VALUES. The first two are each backend's account of its
 *   own memory (one may interleave a stream the other keeps separate), and depth's numeric ENCODING is each
 *   backend's own convention - the seam promises the nearer fragment survives, not the same float. (This backend
 *   folds depth the same way the reference does; the parity suite is what made that a fact instead of an
 *   assumption - see TheNearerFragmentSurvivesOnBothBackends - but a diff there would be a convention change,
 *   not a seam break.) That is the same "measured, not claimed" rule the M6 evidence follows.
 *
 * THE FRAMES ARE HAND-BUILT VALUES, not engine output: `FrameDescription` is the wire, so a case that goes
 * through the engine would be testing the engine twice. What each case says is only what a backend may rely on.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include <vine/graphics/ChainPresets.hpp>
#include <vine/graphics/DepthPolicy.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ImageSource.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vsg/internal/VsgBackend.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::BlendFactor;
using vn::graphics::BlendState;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DepthPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::FrameReport;
using vn::graphics::Geometry;
using vn::graphics::Light;
using vn::graphics::LightType;
using vn::graphics::LightValue;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PassRenderState;
using vn::graphics::PresentOutcome;
using vn::graphics::RenderBackend;
using vn::graphics::DrawItem;
using vn::graphics::RenderState;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;
using vn::graphics::DrawKind;
using vn::intrusive_ptr;

namespace
{

/// @brief Side of the picture in pixels: the size the reference pixel cases use, so a diff covers the frame.
constexpr int kSide = 64;

/// @brief The content ABI's camera push: `projection` then `modelView`, both mat4 (see ShaderAbi / PushRange).
constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} vn_pc;

layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} vn_draw;

void main()
{
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(in_position, 1.0);
    gl_PointSize = vn_draw.params.y;
}
)";

/// @brief Writes the material's base colour: set 0 binding 0 is the material block.
constexpr const char8_t* kFragmentStage = u8R"(
#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 base_color;
    vec4 emissive;
    vec4 params;
    uvec4 flags;
} vn_material;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(vn_material.base_color.rgb, 1.0);
}
)";

/// @brief Samples the first texture slot: binding 1 is the base-colour map.
constexpr const char8_t* kTextureFragment = u8R"(
#version 450
layout(set = 0, binding = 1) uniform sampler2D vn_tex;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texture(vn_tex, vec2(0.5, 0.5));
}
)";

/// @brief Writes the material's colour with the drawable's opacity as alpha (the blend case).
constexpr const char8_t* kAlphaFragment = u8R"(
#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 base_color;
    vec4 emissive;
    vec4 params;
    uvec4 flags;
} vn_material;

layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} vn_draw;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(vn_material.base_color.rgb, vn_draw.params.x * vn_material.base_color.a);
}
)";

/// @brief Builds a two-stage program (a fresh object per call, like every SDK factory).
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgramWith(const char8_t* fragment_source, const char8_t* name)
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(String(name));
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(fragment_source), String(u8"main") });
    return program;
}

/// @brief Builds an opaque material of one colour.
[[nodiscard]] intrusive_ptr<Material> makeMaterial(const Colorf& color)
{
    auto material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(color);
    return material;
}

/// @brief Builds a triangle in clip space at depth @p z, indexed, as the one primitive of its geometry.
///
/// The z values are inside (-1, 1): the identity projection leaves clip z as authored, and a fragment at the far
/// plane would fail every depth comparison.
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle(float z)
{
    std::vector<float> positions{
        -0.75F, -0.75F, z, // bottom left
        0.75F,  -0.75F, z, // bottom right
        0.0F,   0.75F,  z, // top
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));

    // Installing positions stated a default non-indexed draw; the triangle is drawn through its index stream, so the
    // default is replaced instead of appended to (see Geometry::addPrimitive).
    vn::graphics::Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = vn::graphics::Primitive::IndexSpan::whole(intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

/// @brief A triangle carrying the channels the ENGINE's vertex stages read (positions, normals and a UV pair).
///
/// The engine's forward vertex stage declares positions and normals unconditionally and its texcoord slot under
/// `VN_TEXTURED`; a frame that runs the engine's own program must feed all three.
[[nodiscard]] intrusive_ptr<Geometry> makeEngineTriangle()
{
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    })));
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
    })));
    geometry->setTexcoords2(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        0.0F, 0.0F,
        1.0F, 0.0F,
        0.5F, 1.0F,
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief A triangle carrying exactly the two channels the ENGINE's G-buffer stage reads: positions and normals.
///
/// The deferred G-buffer program writes the view normal and never consumes a texcoord or a vertex colour, so the
/// geometry states only what that program declares. A channel the pipeline supplies and the shader ignores is legal,
/// but it is the kind of noise validation reports, and noise hides the next real finding.
[[nodiscard]] intrusive_ptr<Geometry> makeGbufferTriangle()
{
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    })));
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief A screen-filling (or strip) QUAD at depth @p z, with the normals the engine's lit stages read.
///
/// The shadow case needs a surface to receive a shadow and a caster to cast it; both are stated in clip space, so the
/// identity camera on both sides means every number is the value the hardware compares.
[[nodiscard]] intrusive_ptr<Geometry> makeEngineQuad(float x0, float x1, float z)
{
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

/// @brief A camera whose matrices are identities, so the triangle's clip-space positions ARE the picture.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    return camera;
}

/// @brief One collected command: @p geometry through @p program, shaded with @p material (or none when null).
[[nodiscard]] DrawItem commandOf(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<ShaderProgram>& program, const intrusive_ptr<Material>& material)
{
    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    if (material != nullptr) {
        command.material             = material;
        command.material_description = material->description();
    }
    return command;
}

/// @brief The shared, immutable list a frame's draw packet carries.
[[nodiscard]] std::shared_ptr<const std::vector<DrawItem>> commandsOf(std::vector<DrawItem> commands)
{
    return std::make_shared<const std::vector<DrawItem>>(std::move(commands));
}

/// @brief The draw packet that renders @p commands through the identity camera, announcing @p lights.
[[nodiscard]] DrawPacket sceneDraw(std::shared_ptr<const std::vector<DrawItem>> commands,
                                        vn::graphics::LightSetPtr                         lights = nullptr)
{
    DrawPacket draw;
    draw.kind         = DrawKind::Scene;
    draw.view.camera  = identityCamera();
    draw.commands     = std::move(commands);
    draw.light_values = std::move(lights);
    return draw;
}

/// @brief A frame with ONE pass: it names @p target, optionally clears it, and issues @p draws.
///
/// @p inputs is what the pass DECLARES to sample (a produced target for a full-screen draw, an imported texture for a
/// material map): the reference backend binds its sampler from that declaration, so a frame that samples an image
/// without declaring it is incomplete - the same rule the engine follows when it fills FrameDescription::Pass.
[[nodiscard]] FrameDescription onePass(RenderTarget* target,
                                       std::optional<ClearPolicy> clear,
                                       DepthPolicy depth,
                                       std::vector<DrawPacket> draws,
                                       std::vector<vn::graphics::ImageSource> inputs = {})
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass   = vn::test::framePassForTest();
    entry.order  = 0;
    entry.inputs = std::move(inputs);

    PassDraw pass;
    pass.target       = target;
    pass.clear        = std::move(clear);
    pass.state.depth  = depth;
    pass.packets        = std::move(draws);
    entry.draw        = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief What one backend answered for one frame: its record and its picture.
struct Ran {
    PresentOutcome            present{ PresentOutcome::Skipped };
    FrameReport               report{};
    std::vector<std::uint8_t> pixels{};
    bool                      read{ false };
};

/// @brief Runs @p frame on @p backend and reads @p target's colour attachment 0 back.
[[nodiscard]] Ran run(RenderBackend& backend, const FrameDescription& frame, const RenderTarget* target)
{
    Ran ran;
    ran.present = backend.executeFrame(frame, nullptr);
    ran.report  = backend.frameReport();
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Ok;
    ran.read    = backend.readColorBuffer(target, 0, ran.pixels, &why);
    if (!ran.read) {
        std::printf("[parity] the readback was refused (why=%d)\n", static_cast<int>(why));
    }
    return ran;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback of the parity target.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kSide + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

/// @brief Asserts that @p candidate recorded the same frame shape as @p reference, and both the claimed numbers.
void expectTheSameRecord(const Ran& reference, const Ran& candidate, std::uint64_t passes, std::uint64_t packets, const char* what)
{
    SCOPED_TRACE(what);
    EXPECT_EQ(reference.present, PresentOutcome::Skipped) << "an off-screen frame presents nowhere";
    EXPECT_EQ(candidate.present, reference.present) << "the same frame is answered the same way";

    ASSERT_TRUE(reference.report.counters.has_value()) << "the reference backend keeps frame counters";
    ASSERT_TRUE(candidate.report.counters.has_value()) << "this backend keeps frame counters";
    EXPECT_EQ(reference.report.counters->passes, passes) << "the reference backend's pass scopes";
    EXPECT_EQ(candidate.report.counters->passes, reference.report.counters->passes)
        << "the same frame is the same number of pass scopes";
    EXPECT_EQ(reference.report.counters->draw_packets, packets) << "the reference backend's draw packets";
    EXPECT_EQ(candidate.report.counters->draw_packets, reference.report.counters->draw_packets)
        << "the same frame is the same number of draw packets";
    EXPECT_EQ(candidate.report.counters->invalid_schedules, 0U);
    std::printf("[parity] %s: passes=%llu draw_packets=%llu (both backends)\n",
                what,
                static_cast<unsigned long long>(candidate.report.counters->passes),
                static_cast<unsigned long long>(candidate.report.counters->draw_packets));
}

/// @brief Asserts that @p candidate's picture is byte-identical to @p reference's, printing the first differences.
void expectTheSamePicture(const Ran& reference, const Ran& candidate, const char* what)
{
    SCOPED_TRACE(what);
    ASSERT_TRUE(reference.read) << "the reference picture must read back";
    ASSERT_TRUE(candidate.read) << "this picture must read back";
    ASSERT_EQ(reference.pixels.size(), candidate.pixels.size()) << "the two readbacks describe different pictures";

    std::size_t differing = 0;
    bool        printed   = false;
    for (std::size_t i = 0; i + 3U < reference.pixels.size(); i += 4U) {
        const bool same = reference.pixels[i + 0U] == candidate.pixels[i + 0U] && reference.pixels[i + 1U] == candidate.pixels[i + 1U] &&
                          reference.pixels[i + 2U] == candidate.pixels[i + 2U] && reference.pixels[i + 3U] == candidate.pixels[i + 3U];
        if (same) {
            continue;
        }
        ++differing;
        if (!printed && differing <= 8U) {
            const int x = static_cast<int>((i / 4U) % static_cast<std::size_t>(kSide));
            const int y = static_cast<int>((i / 4U) / static_cast<std::size_t>(kSide));
            std::printf("[parity] %s: first diff at (%d,%d): reference=(%u,%u,%u,%u) candidate=(%u,%u,%u,%u)\n",
                        what,
                        x,
                        y,
                        static_cast<unsigned>(reference.pixels[i + 0U]),
                        static_cast<unsigned>(reference.pixels[i + 1U]),
                        static_cast<unsigned>(reference.pixels[i + 2U]),
                        static_cast<unsigned>(reference.pixels[i + 3U]),
                        static_cast<unsigned>(candidate.pixels[i + 0U]),
                        static_cast<unsigned>(candidate.pixels[i + 1U]),
                        static_cast<unsigned>(candidate.pixels[i + 2U]),
                        static_cast<unsigned>(candidate.pixels[i + 3U]));
            printed = true;
        }
    }
    std::printf("[parity] %s: %zu differing pixel(s) of %zu\n", what, differing, reference.pixels.size() / 4U);
    EXPECT_EQ(differing, 0U) << "the two backends drew different pixels for the same frame";
}

} // namespace

/// @brief The two devices every case drives, and the targets both of them announce.
///
/// THE OFF-SCREEN FORM ON BOTH SIDES (`DevicePresentation::None`): a parity case must not depend on a display
/// server, and the frame value it draws presents nowhere. Each backend builds its OWN attachments for the shared
/// `RenderTarget` description - announcing one target to two devices is two announcements, which is exactly the
/// rule the seam has (a device holds a target BY POINTER and owns the objects it made for it).
class BackendParityTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        // BOTH DEVICES ARE THE CODE COMPILED INTO THIS BINARY, not the registry's answer: the registry is a
        // name -> factory map, and any plugin a later suite loads overwrites an entry (measured: once
        // GfxBackendVsgPluginTest had run, "vsg" resolved to that module's build instead of this one). A parity
        // diff has to compare the tree it was built from, or the same case means different code per test order.
        const DeviceDesc desc{ DevicePresentation::None, nullptr };
        vsg = intrusive_ptr<RenderBackend>(new vn::backend_vsg::VsgBackend(desc));
        vk  = intrusive_ptr<RenderBackend>(new vn::backend_vk::Device(desc));
        if (!vsg->initialize()) {
            GTEST_SKIP() << "the reference (vsg) device cannot come up on this machine";
        }
        if (!vk->initialize()) {
            GTEST_SKIP() << "the vk device cannot come up on this machine";
        }
        // WHAT EACH DEVICE SAYS WHILE A CASE RUNS: a divergence is usually a refusal (a skipped draw, a target it
        // would not build), and a refusal is a sentence on this channel rather than a silently different picture.
        vsg->setDiagnosticSink([](const vn::graphics::RenderDiagnostic& diagnostic) {
            std::printf("[parity/vsg] severity=%d category=%d %s\n",
                        static_cast<int>(diagnostic.severity),
                        static_cast<int>(diagnostic.category),
                        std::string(reinterpret_cast<const char*>(diagnostic.message.data()), diagnostic.message.size()).c_str());
        });
        vk->setDiagnosticSink([](const vn::graphics::RenderDiagnostic& diagnostic) {
            std::printf("[parity/vk] severity=%d category=%d %s\n",
                        static_cast<int>(diagnostic.severity),
                        static_cast<int>(diagnostic.category),
                        std::string(reinterpret_cast<const char*>(diagnostic.message.data()), diagnostic.message.size()).c_str());
        });
    }

    void TearDown() override
    {
        // The devices were CREATED here, so they are taken down here (see RenderDevice::initialize) - and before
        // the targets die, because each holds the targets it was told about by pointer.
        if (vsg != nullptr) {
            vsg->shutdown();
        }
        if (vk != nullptr) {
            vk->shutdown();
        }
    }

    /// @brief Creates a 64x64 RGBA8 + D24 target and announces it to BOTH devices.
    ///
    /// Announced here rather than per frame: that is the host's resource announcement (see RenderDevice), and a
    /// backend that built attachments lazily would still be told before anything draws into them.
    RenderTarget* target()
    {
        auto made = intrusive_ptr<RenderTarget>(new RenderTarget());
        made->setName(String(u8"parity_target"));
        made->attachColor(RenderTarget::ColorFormat::RGBA8);
        made->attachDepth(RenderTarget::DepthFormat::D24);
        made->setSize(kSide, kSide);
        vsg->setRenderTarget(made.get());
        vk->setRenderTarget(made.get());
        targets.push_back(made);
        return made.get();
    }

    /// @brief Creates a 64x64 DEPTH-ONLY target that states whose shadow map it is, and announces it to both devices.
    RenderTarget* shadowMap(const intrusive_ptr<Light>& light)
    {
        auto made = intrusive_ptr<RenderTarget>(new RenderTarget());
        made->setName(String(u8"parity_shadow_map"));
        made->attachDepth(RenderTarget::DepthFormat::D24);
        made->setSize(kSide, kSide);
        made->setShadowOf(light);
        made->setProducerViewProjection(vn::math::Mat4d{});
        vsg->setRenderTarget(made.get());
        vk->setRenderTarget(made.get());
        targets.push_back(made);
        return made.get();
    }

    /// @brief Creates the canonical 64x64 four-attachment G-buffer (RGBA16F x4 + D24) and announces it to BOTH devices.
    ///
    /// The shape is `defaultGbufferTarget`'s (the Deferred preset's own), but the target is built here rather than
    /// taken from that preset: the parity suite owns the targets it announces, and a preset-owned one would be a
    /// second announcement path in the same case. What the case needs from the shape is that the four rows are
    /// DISTINCT attachments of one target, so a backend that binds them by index can be checked against one that
    /// binds them another way.
    RenderTarget* gbufferTarget()
    {
        auto made = intrusive_ptr<RenderTarget>(new RenderTarget());
        made->setName(String(u8"parity_gbuffer"));
        for (int attachment = 0; attachment < 4; ++attachment) {
            made->attachColor(RenderTarget::ColorFormat::RGBA16F);
        }
        made->attachDepth(RenderTarget::DepthFormat::D24);
        made->setSize(kSide, kSide);
        vsg->setRenderTarget(made.get());
        vk->setRenderTarget(made.get());
        targets.push_back(made);
        return made.get();
    }

    // Declaration order matters: the devices must die BEFORE the targets they hold (members destroy in reverse).
    std::vector<intrusive_ptr<RenderTarget>> targets;
    intrusive_ptr<RenderBackend>             vsg;
    intrusive_ptr<RenderBackend>             vk;
};

TEST_F(BackendParityTest, TheSameClearIsTheSamePicture)
{
    // THE SMALLEST FRAME: one pass, one clear, no draw packet. What it pins is that both backends clear to the
    // SAME stored bytes - the state every later case starts from.
    RenderTarget* const shared = target();

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.25F, 0.0F, 1.0F);

    const FrameDescription frame = onePass(shared, clear, DepthPolicy::TestAndWrite, {});

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 1, 0, "clear-only frame");
    expectTheSamePicture(reference, candidate, "clear-only frame");
    // "BOTH BLACK" MUST NOT PASS: the picture is the pass' clear colour (0.25 linear reads back as 64 +- 1).
    const std::array<int, 4> centre = pixelAt(reference.pixels, kSide / 2, kSide / 2);
    EXPECT_LE(std::abs(centre[0]), 1);
    EXPECT_LE(std::abs(centre[1] - 64), 1);
    EXPECT_LE(std::abs(centre[2]), 1);
    EXPECT_EQ(centre[3], 255);
}

TEST_F(BackendParityTest, ADepthOnlyClearKeepsThePictureOnBothBackends)
{
    // THE HUD LAYER'S CLEAR (see ClearPolicy's two dimensions): a later pass into the SAME target loads the colour and
    // resets only the depth - "the marker owns the depth inside its box, the scene underneath stays". What this pins:
    // the picture the first pass drew is still there (the colour was LOADed, not cleared), the depth really was reset
    // (the second pass is recorded with no draws and a clear of its own), and both backends answer the same picture.
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"parity_depth_only_clear");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    // Two passes are two identities (a backend keys retained per-pass state by the object), so the frame owns them.
    const intrusive_ptr<vn::graphics::RenderPass> painting_identity(new vn::graphics::RenderPass());
    const intrusive_ptr<vn::graphics::RenderPass> overlay_identity(new vn::graphics::RenderPass());

    FrameDescription frame;

    FrameDescription::Pass painting;
    painting.pass  = painting_identity.get();
    painting.order = 0;
    PassDraw paint;
    paint.target      = shared;
    paint.clear       = ClearPolicy{ Colorf(0.0F, 0.0F, 0.0F, 1.0F), true };
    paint.state.depth = DepthPolicy::TestAndWrite;
    paint.packets.push_back(sceneDraw(commandsOf({ commandOf(geometry, program, red) })));
    painting.draw = std::move(paint);
    frame.passes.push_back(std::move(painting));

    FrameDescription::Pass overlay;
    overlay.pass  = overlay_identity.get();
    overlay.order = 1;
    PassDraw cleaned;
    cleaned.target      = shared;
    cleaned.clear       = ClearPolicy::depthOnly(); // the colour is LOADed and the depth is CLEARed
    cleaned.state.depth = DepthPolicy::TestAndWrite;
    overlay.draw = std::move(cleaned);
    frame.passes.push_back(std::move(overlay));

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 2, 1, "depth-only clear over a drawn picture");
    expectTheSamePicture(reference, candidate, "depth-only clear over a drawn picture");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the colour was LOADed: the triangle the first pass drew is still the picture";
}

TEST_F(BackendParityTest, TheSameTriangleIsTheSamePicture)
{
    // ONE OPAQUE DRAW through the engine ABI: the vertex stage reads the camera push and the draw block, the
    // fragment stage writes the material's base colour. The centre is the fragment colour, the corner the clear.
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"parity_material");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription frame =
        onePass(shared, clear, DepthPolicy::TestAndWrite, { sceneDraw(commandsOf({ commandOf(geometry, program, red) })) });

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 1, 1, "opaque triangle");
    expectTheSamePicture(reference, candidate, "opaque triangle");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the reference picture is not the fragment stage's colour";
    EXPECT_EQ(pixelAt(reference.pixels, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 })) << "the reference picture is not the pass' clear";
    EXPECT_EQ(pixelAt(candidate.pixels, kSide / 2, kSide / 2), pixelAt(reference.pixels, kSide / 2, kSide / 2));
}

TEST_F(BackendParityTest, APrepareReportsTheShapeBothBackendsThenRecord)
{
    // T41: `PrepareReport.passes` / `draw_packets` / `draws` are the WORKLOAD of the frame (see vn::graphics::FrameCounts), and a
    // recorded frame's `FrameCounters` follows the same rule. The three passes below are chosen so every half of the
    // rule is exercised at once:
    //   - a CLEAR-ONLY pass: no draw packet, but it clears - a real pass a recorder enters;
    //   - a pass whose only packet issues NOTHING (no camera): it announced neither a draw nor a clear;
    //   - a pass that draws.
    // The asked shape is therefore TWO passes and ONE packet carrying ONE draw, and both backends must say so - and
    // then record it.
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"parity_prepare_shape");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.25F, 1.0F);

    DrawPacket idle = sceneDraw(commandsOf({ commandOf(geometry, program, red) }));
    idle.view.camera     = CameraSnapshot{}; // present == false: the pass has no view, so it issues no call

    // ONE PASS IDENTITY PER ENTRY (a backend keys retained per-pass state by it; two passes never share one).
    // VALIDATION IS PART OF THE CASE, not a log nobody reads: three passes into one depth-attached target is exactly
    // where a LOAD render-pass variant whose subpass dependency differed from the CLEAR variant's stopped being
    // compatible with the framebuffer they share (VUID-VkRenderPassBeginInfo-renderPass-00904). A backend that brings
    // that back fails here.
    auto reported = std::make_shared<int>(0);
    vk->setDiagnosticSink([reported](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.severity == vn::graphics::DiagnosticSeverity::Error ||
            diagnostic.severity == vn::graphics::DiagnosticSeverity::Warning) {
            std::printf("[parity/vk] severity=%d category=%d %s\n",
                        static_cast<int>(diagnostic.severity),
                        static_cast<int>(diagnostic.category),
                        std::string(reinterpret_cast<const char*>(diagnostic.message.data()), diagnostic.message.size()).c_str());
            ++*reported;
        }
    });

    std::vector<FrameDescription::Pass>                  frame_passes;
    std::vector<intrusive_ptr<vn::graphics::RenderPass>> pass_objects;
    auto add = [&](std::optional<ClearPolicy> announced, std::vector<DrawPacket> draws) {
        pass_objects.push_back(intrusive_ptr<vn::graphics::RenderPass>(new vn::graphics::RenderPass()));
        FrameDescription::Pass entry;
        entry.pass  = pass_objects.back().get();
        entry.order = static_cast<int>(pass_objects.size()) - 1;
        PassDraw pass;
        pass.target      = shared;
        pass.clear       = std::move(announced);
        pass.state.depth = DepthPolicy::TestAndWrite;
        pass.packets       = std::move(draws);
        entry.draw       = std::move(pass);
        frame_passes.push_back(std::move(entry));
    };

    add(clear, {});
    add(std::nullopt, { idle });
    add(std::nullopt, { sceneDraw(commandsOf({ commandOf(geometry, program, red) })) });

    FrameDescription frame;
    frame.passes = std::move(frame_passes);

    const vn::graphics::PrepareReport reference = vsg->prepareFrame(frame, nullptr);
    const vn::graphics::PrepareReport candidate = vk->prepareFrame(frame, nullptr);

    ASSERT_TRUE(reference.prepared()) << "the reference backend would not prepare the frame";
    ASSERT_TRUE(candidate.prepared()) << "the direct backend would not prepare the frame";
    EXPECT_EQ(reference.passes, 2U) << "the reference backend did not report the asked shape";
    EXPECT_EQ(candidate.passes, reference.passes) << "the two backends reported different asked shapes";
    EXPECT_EQ(reference.draw_packets, 1U) << "only the packet that issues something counts";
    EXPECT_EQ(candidate.draw_packets, reference.draw_packets) << "the two backends counted different draw packets";
    EXPECT_EQ(reference.draws, 1U) << "the one effective packet submits one draw";
    EXPECT_EQ(candidate.draws, reference.draws) << "the two backends counted different draws";

    const Ran reference_run = run(*vsg, frame, shared);
    const Ran candidate_run = run(*vk, frame, shared);

    // THE RECORDED FRAME AGREES WITH THE ASKED SHAPE: that is the promise `PrepareReport` makes.
    expectTheSameRecord(reference_run, candidate_run, 2, 1, "clear + idle + drawing");
    ASSERT_TRUE(candidate_run.report.counters.has_value());
    EXPECT_EQ(candidate_run.report.counters->passes, candidate.passes) << "the recorded pass count moved from the asked one";
    EXPECT_EQ(candidate_run.report.counters->draw_packets, candidate.draw_packets) << "the recorded packet count moved from the asked one";
    EXPECT_EQ(candidate_run.report.counters->draws, candidate.draws) << "the recorded draw count moved from the asked one";
    EXPECT_EQ(*reported, 0) << "the direct backend reported a problem while preparing and recording the frame";
}

TEST_F(BackendParityTest, ADrawPacketCarriesItsDrawsAndBothBackendsCountThem)
{
    // THE TWO NUMBERS ANSWER DIFFERENT QUESTIONS (see FrameCounts): one draw packet carrying two draws is ONE draw
    // packet and TWO draws - the number a profiler calls draw calls. Both backends must say so, and the asked
    // counts must be the recorded ones.
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"parity_draws");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);

    const FrameDescription frame =
        onePass(shared,
                clear,
                DepthPolicy::TestAndWrite,
                { sceneDraw(commandsOf({ commandOf(geometry, program, red), commandOf(geometry, program, red) })) });

    const vn::graphics::PrepareReport reference = vsg->prepareFrame(frame, nullptr);
    const vn::graphics::PrepareReport candidate = vk->prepareFrame(frame, nullptr);
    ASSERT_TRUE(reference.prepared()) << "the reference backend would not prepare the frame";
    ASSERT_TRUE(candidate.prepared()) << "the direct backend would not prepare the frame";
    EXPECT_EQ(reference.draw_packets, 1U) << "one view's list is one draw packet";
    EXPECT_EQ(reference.draws, 2U) << "the packet carries two draws";
    EXPECT_EQ(candidate.draw_packets, reference.draw_packets) << "the two backends counted different draw packets";
    EXPECT_EQ(candidate.draws, reference.draws) << "the two backends counted different draws";

    const Ran reference_run = run(*vsg, frame, shared);
    const Ran candidate_run = run(*vk, frame, shared);

    expectTheSameRecord(reference_run, candidate_run, 1, 1, "one packet over two draws");
    ASSERT_TRUE(reference_run.report.counters.has_value());
    ASSERT_TRUE(candidate_run.report.counters.has_value());
    EXPECT_EQ(reference_run.report.counters->draws, 2U) << "the reference backend lost a draw";
    EXPECT_EQ(candidate_run.report.counters->draws, reference_run.report.counters->draws)
        << "the same frame is the same number of draws";
    EXPECT_EQ(candidate_run.report.counters->draw_packets, 1U) << "one view's list is one draw packet";
}

TEST_F(BackendParityTest, TheNearerFragmentSurvivesOnBothBackends)
{
    // DEPTH ORDER, with the DISTANT triangle drawn LAST: if either backend ignored the depth test, the far red would
    // overwrite the near green. NEAR IS THE SMALLER CLIP Z here (the SDK projection puts the near plane at clip
    // z = -1), which is the fact this case exists to check: a backend that only folded Y used to keep the FARTHER
    // fragment, and its own tests - written against its own convention - could not see it (M7).
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"parity_depth");
    const intrusive_ptr<Geometry>      near_z   = makeTriangle(-0.25F);
    const intrusive_ptr<Geometry>      far_z    = makeTriangle(0.75F);
    const intrusive_ptr<Material>      green    = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription frame     = onePass(shared,
                                               clear,
                                               DepthPolicy::TestAndWrite,
                                               { sceneDraw(commandsOf({ commandOf(near_z, program, green), commandOf(far_z, program, red) })) });

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 1, 1, "near then far");
    expectTheSamePicture(reference, candidate, "near then far");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 0, 255, 0, 255 }))
        << "the reference backend did not keep the nearer fragment";
}

TEST_F(BackendParityTest, TheSameHalfOpacityDrawBlendsTheSame)
{
    // BLEND, with the engine's order: an opaque red triangle, then a half-opacity green one over it. One pair of
    // blend factors serves all four channels (see BlendState), so the expected centre is (127, 128, 0, 191).
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kAlphaFragment, u8"parity_alpha");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<Material>      green    = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    DrawItem over = commandOf(geometry, program, green);
    over.opacity       = 0.5F;
    over.authored_state.blend =
        BlendState{ true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha };

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription blending =
        onePass(shared, clear, DepthPolicy::Disabled, { sceneDraw(commandsOf({ commandOf(geometry, program, red), over })) });

    const Ran reference = run(*vsg, blending, shared);
    const Ran candidate = run(*vk, blending, shared);

    expectTheSameRecord(reference, candidate, 1, 1, "half-opacity overlay");
    expectTheSamePicture(reference, candidate, "half-opacity overlay");
    const std::array<int, 4> centre = pixelAt(reference.pixels, kSide / 2, kSide / 2);
    EXPECT_LE(std::abs(centre[0] - 127), 2);
    EXPECT_LE(std::abs(centre[1] - 128), 2);
    EXPECT_EQ(centre[2], 0);
    EXPECT_LE(std::abs(centre[3] - 191), 2);
}

TEST_F(BackendParityTest, TheSameFullscreenCopySamplesTheSameColours)
{
    // THE FULL-SCREEN PATH: a scene frame fills the source, a screen frame samples it into the destination. Two
    // frames, each compared as a record and as pixels - the copy must reproduce the drawn centre AND the cleared
    // corner, which is what tells "sampled the source" from "cleared the destination".
    RenderTarget* const source      = target();
    RenderTarget* const destination = target();

    const intrusive_ptr<ShaderProgram> scene  = makeProgramWith(kFragmentStage, u8"parity_screen_source");
    const intrusive_ptr<ShaderProgram> copy   = vn::graphics::fullscreenCopyProgram(0);
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<ClearPolicy> source_clear = ClearPolicy{};
    source_clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription scene_frame =
        onePass(source, source_clear, DepthPolicy::TestAndWrite, { sceneDraw(commandsOf({ commandOf(geometry, scene, red) })) });

    DrawPacket screen;
    screen.kind                = DrawKind::Fullscreen;
    screen.view.camera         = identityCamera();
    screen.source              = source;
    screen.program             = copy;
    screen.program_description = copy->description();
    const FrameDescription screen_frame =
        onePass(destination, std::nullopt, DepthPolicy::Disabled, { screen }, { vn::graphics::ImageSource::producedTarget(source) });

    const Ran reference_scene = run(*vsg, scene_frame, source);
    const Ran candidate_scene = run(*vk, scene_frame, source);
    expectTheSameRecord(reference_scene, candidate_scene, 1, 1, "screen source frame");
    expectTheSamePicture(reference_scene, candidate_scene, "screen source frame");

    const Ran reference = run(*vsg, screen_frame, destination);
    const Ran candidate = run(*vk, screen_frame, destination);
    expectTheSameRecord(reference, candidate, 1, 1, "screen copy frame");
    expectTheSamePicture(reference, candidate, "screen copy frame");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the reference copy did not sample the source's drawn pixel";
    EXPECT_EQ(pixelAt(reference.pixels, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 })) << "the reference copy did not sample the source's cleared corner";
}

TEST_F(BackendParityTest, TheSameTextureIsSampledTheSame)
{
    // THE TEXTURE PATH: a 1x1 blue base-colour map is uploaded and sampled at set 0 binding 1. A 1x1 map has no
    // filtering freedom at all, so "the texture arrived" is a byte-exact claim on both backends.
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kTextureFragment, u8"parity_texture");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);

    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0]                         = std::byte{ 0U };
    texel[1]                         = std::byte{ 0U };
    texel[2]                         = std::byte{ 255U };
    texel[3]                         = std::byte{ 255U };
    auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);

    const intrusive_ptr<Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    material->setTexture(TextureSlot::BaseColor, texture);

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription frame     = onePass(shared,
                                               clear,
                                               DepthPolicy::TestAndWrite,
                                               { sceneDraw(commandsOf({ commandOf(geometry, program, material) })) },
                                               { vn::graphics::ImageSource::imported(texture.get()) });

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 1, 1, "sampled texture");
    expectTheSamePicture(reference, candidate, "sampled texture");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 0, 0, 255, 255 }))
        << "the reference backend did not sample the base-colour texture";
}

TEST_F(BackendParityTest, TheEngineUnlitProgramDrawsTheSamePictureOnBothBackends)
{
    // THE ENGINE'S OWN TEXT, through the seam: `unlitForwardProgram()` declares the material block, five texture
    // slots, the lights, the draw block AND the shadow map + block (its forward source inserts that ABI
    // unconditionally), and its texcoord slot only under `VN_TEXTURED`. "The engine's programs run on this backend"
    // therefore means every binding, the variant's names and the vertex layout agree - and the picture is where all
    // three meet. ONE frame, the same value, two backends.
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = vn::graphics::unlitForwardProgram();
    const intrusive_ptr<Geometry>      geometry = makeEngineTriangle();

    // An sRGB map (a colour slot's own colour space) of pure green: both backends decode it in hardware, so the two
    // pictures being byte-identical also says the format reached the image on both sides.
    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Srgb));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0]                         = std::byte{ 0U };
    texel[1]                         = std::byte{ 255U };
    texel[2]                         = std::byte{ 0U };
    texel[3]                         = std::byte{ 255U };
    auto texture = intrusive_ptr<vn::graphics::Texture2D>(new vn::graphics::Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Srgb));
    texture->setImage(image);

    const intrusive_ptr<Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    material->setTexture(vn::graphics::TextureSlot::BaseColor, texture);

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription frame     = onePass(shared,
                                               clear,
                                               DepthPolicy::TestAndWrite,
                                               { sceneDraw(commandsOf({ commandOf(geometry, program, material) })) },
                                               { vn::graphics::ImageSource::imported(texture.get()) });

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 1, 1, "engine unlit program, textured");
    expectTheSamePicture(reference, candidate, "engine unlit program, textured");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 0, 255, 0, 255 }))
        << "the reference backend did not sample the engine's base-colour map";
}

TEST_F(BackendParityTest, APassWithNoEffectIsNotAScopeOnEitherBackend)
{
    // A RECORD IS WHAT HAD AN EFFECT (see FrameCounters / the reference recorder): an entry that announces neither a
    // clear nor a draw packet records nothing on either backend - and the target it never named was never built, so
    // neither backend has a picture to answer with.
    RenderTarget* const shared = target();

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;
    frame.passes.push_back(std::move(entry));

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 0, 0, "pass with no effect");
    EXPECT_FALSE(reference.read) << "a pass that announces no target cannot have produced a picture";
    EXPECT_EQ(candidate.read, reference.read) << "the same frame is refused the same way";
}

TEST_F(BackendParityTest, AnEntryThatNamesNoPassIsNothingToRunOnBothBackends)
{
    // THE SDK'S RULE (see RenderFrameExecutor::executeFrame): an entry with a null pass is NOTHING TO RUN. A frame
    // that would otherwise draw must do nothing at all - no pass scope, no draw packet, no picture.
    RenderTarget* const shared = target();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"parity_no_pass");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass  = nullptr;
    entry.order = 0;
    PassDraw pass;
    pass.target       = shared;
    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.clear        = clear;
    pass.state.depth  = DepthPolicy::TestAndWrite;
    pass.packets.push_back(sceneDraw(commandsOf({ commandOf(geometry, program, red) })));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 0, 0, "entry without a pass");
    EXPECT_FALSE(reference.read) << "an entry that names no pass must not have drawn anything";
    EXPECT_EQ(candidate.read, reference.read) << "the same frame is refused the same way";
}

TEST_F(BackendParityTest, TheSameShadowMapIsTheSamePictureOnBothBackends)
{
    // THE WHOLE SHADOW PATH, as ONE frame value: a DEPTH-ONLY pass writes a map (no colour attachment at all), the
    // shading pass declares that target as an input, and the engine's own forward program samples it with the
    // `VineShadowBlock` each backend packs from the map's statement (whose it is, and how to read it).
    //
    // WHAT A DIFF HERE CAN SEE, and what the single-backend cases cannot: the block's matrix has to be
    // `light clip <- world * world <- view` (a sign or a transpose error samples a mirrored texel), the map's depth has
    // to be compared in the SAME reverse-Z direction on both sides, and the sampler has to pick the SAME texel (a
    // filtered read moves every silhouette). Each of those is invisible in one backend's own picture.
    RenderTarget* const picture = target();

    auto light = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    light->setColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    light->setIntensity(1.0F);
    light->setCastShadow(true);

    RenderTarget* const map = shadowMap(light);

    const intrusive_ptr<ShaderProgram> program  = vn::graphics::forwardProgram();
    const intrusive_ptr<Geometry>      ground   = makeEngineQuad(-1.0F, 1.0F, 0.5F);
    const intrusive_ptr<Geometry>      occluder = makeEngineQuad(-1.0F, -0.25F, 0.0F);
    const intrusive_ptr<Material>      material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));

    // Two passes are two identities (a backend keys per-pass retained state by the object), so the frame owns them.
    const intrusive_ptr<vn::graphics::RenderPass> shadow_identity(new vn::graphics::RenderPass());
    const intrusive_ptr<vn::graphics::RenderPass> main_identity(new vn::graphics::RenderPass());

    std::vector<LightValue> lights{ light->value() };

    FrameDescription frame;

    FrameDescription::Pass shadow_entry;
    shadow_entry.pass  = shadow_identity.get();
    shadow_entry.order = 0;
    PassDraw shadow_pass;
    shadow_pass.target      = map;
    shadow_pass.clear       = ClearPolicy{};
    shadow_pass.state.depth = DepthPolicy::TestAndWrite;
    shadow_pass.packets.push_back(
        sceneDraw(commandsOf({ commandOf(ground, program, material), commandOf(occluder, program, material) })));
    shadow_entry.draw = std::move(shadow_pass);
    frame.passes.push_back(std::move(shadow_entry));

    FrameDescription::Pass main_entry;
    main_entry.pass   = main_identity.get();
    main_entry.order  = 1;
    main_entry.inputs = { vn::graphics::ImageSource::producedTarget(map) };

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    PassDraw main_pass;
    main_pass.target      = picture;
    main_pass.clear       = clear;
    main_pass.state.depth = DepthPolicy::TestAndWrite;
    main_pass.packets.push_back(sceneDraw(commandsOf({ commandOf(ground, program, material) }), vn::graphics::LightSet::create(std::move(lights))));
    main_entry.draw = std::move(main_pass);
    frame.passes.push_back(std::move(main_entry));

    const Ran reference = run(*vsg, frame, picture);
    const Ran candidate = run(*vk, frame, picture);

    expectTheSameRecord(reference, candidate, 2, 2, "the same shadow frame");
    expectTheSamePicture(reference, candidate, "the same shadow frame");

    // AND THE SHADOW IS THERE: the strip behind the caster is darker than the strip beside it on BOTH sides, so the
    // diff above compares two pictures that actually carry a shadow rather than two identical unshadowed ones.
    const int shadowed = pixelAt(reference.pixels, kSide / 4, kSide / 2)[1];
    const int lit      = pixelAt(reference.pixels, (3 * kSide) / 4, kSide / 2)[1];
    EXPECT_LT(shadowed, lit) << "the reference backend's own picture has no shadow either";
}

TEST_F(BackendParityTest, TheSameDeferredFrameIsTheSamePictureOnBothBackends)
{
    // THE DEFERRED CHAIN AS ONE FRAME VALUE: a scene pass writes the four-attachment G-buffer through the SDK's
    // G-buffer geometry program, then a SCREEN pass shades that G-buffer through the SDK's deferred lighting program.
    //
    // WHY THIS CASE IS WORTH A SECOND FRAME SHAPE. The single-backend cases prove the chain runs on each backend
    // separately, and each of them agrees with whatever ITS OWN binding rule says. What only a two-backend diff can
    // see: whether attachment N lands where the lighting program's sampler N reads it (a shifted row lights a
    // surface with another surface's albedo), whether the pass' DECLARED input is what the full-screen packet samples (a
    // backend that samples the wrong target still draws a plausible picture), and whether the light block sits at
    // the binding the program declares - an ambient of 0 and a picture of pure black are indistinguishable when the
    // only instrument is one backend's own output.
    RenderTarget* const gbuffer = gbufferTarget();
    RenderTarget* const result  = target();

    const intrusive_ptr<ShaderProgram> geometry_program = vn::graphics::defaultGbufferGeometryProgram();
    const intrusive_ptr<ShaderProgram> light_program    = vn::graphics::defaultDeferredLightProgram();
    ASSERT_NE(geometry_program, nullptr) << "the SDK ships the G-buffer geometry program";
    ASSERT_NE(light_program, nullptr) << "the SDK ships the deferred lighting program";
    ASSERT_EQ(gbuffer->colorCount(), 4) << "the G-buffer is four colour attachments (see defaultGbufferTarget)";

    // The triangle states the two channels the G-buffer stage reads (position and view normal) and nothing else, so
    // the frame's vertex layout and its program declare the same inputs.
    const intrusive_ptr<Geometry> geometry = makeGbufferTriangle();
    // The engine's default surface (metallic 0, roughness 0.5, occlusion 1 - the values the lighting pass reads back
    // out of attachments 1 and 2), so the composite is the ambient term applied to a known albedo.
    const intrusive_ptr<Material> material = makeMaterial(Colorf(0.8F, 0.8F, 0.8F, 1.0F));

    // Two passes are two identities (a backend keys per-pass retained state by the object), so the frame owns them.
    const intrusive_ptr<vn::graphics::RenderPass> gbuffer_identity(new vn::graphics::RenderPass());
    const intrusive_ptr<vn::graphics::RenderPass> light_identity(new vn::graphics::RenderPass());

    FrameDescription frame;

    FrameDescription::Pass gbuffer_entry;
    gbuffer_entry.pass  = gbuffer_identity.get();
    gbuffer_entry.order = 0;
    PassDraw gbuffer_pass;
    gbuffer_pass.target = gbuffer;
    // Attachment 0's clear is stated and the rest are LEFT EMPTY on purpose: an attachment nobody states a colour
    // for is cleared to transparent black, which is the G-buffer's own "nothing was drawn here" - attachment 3's
    // alpha is the lighting program's write mask, so that default is part of the frame's contract (see ClearPolicy).
    gbuffer_pass.clear        = ClearPolicy{};
    gbuffer_pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 0.0F);
    gbuffer_pass.state.depth  = DepthPolicy::TestAndWrite;
    gbuffer_pass.packets.push_back(sceneDraw(commandsOf({ commandOf(geometry, geometry_program, material) })));
    gbuffer_entry.draw = std::move(gbuffer_pass);
    frame.passes.push_back(std::move(gbuffer_entry));

    FrameDescription::Pass light_entry;
    light_entry.pass  = light_identity.get();
    light_entry.order = 1;
    // The DECLARATION is what a backend binds its samplers from: the whole produced target, so the full-screen ABI's
    // "binding N = attachment N" finds all four G-buffer rows (see ImageSource::producedTarget).
    light_entry.inputs.push_back(vn::graphics::ImageSource::producedTarget(gbuffer));

    DrawPacket screen;
    screen.kind                = DrawKind::Fullscreen;
    screen.view.camera         = identityCamera();
    screen.source              = gbuffer;
    screen.program             = light_program;
    screen.program_description = light_program->description();
    // ONE ambient light, so the composite is the albedo the G-buffer stored rather than a black frame: the deferred
    // program multiplies the albedo by the ambient term, which is the only thing that tells "the chain ran" apart
    // from "two targets were cleared".
    LightValue ambient;
    ambient.enabled     = true;
    ambient.type        = LightType::Ambient;
    ambient.color       = Colorf(1.0F, 1.0F, 1.0F, 1.0F);
    ambient.intensity   = 1.0F;
    screen.light_values = vn::graphics::LightSet::create({ ambient });

    PassDraw light_pass;
    light_pass.target       = result;
    light_pass.clear        = ClearPolicy{};
    light_pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    // The destination carries depth and a full-screen triangle sits at the far plane: the pass draws OVER what the
    // target holds, which is the same policy the SDK's own lighting pass states by carrying no depth at all.
    light_pass.state.depth = DepthPolicy::Disabled;
    light_pass.packets.push_back(std::move(screen));
    light_entry.draw = std::move(light_pass);
    frame.passes.push_back(std::move(light_entry));

    const Ran reference = run(*vsg, frame, result);
    const Ran candidate = run(*vk, frame, result);

    expectTheSameRecord(reference, candidate, 2, 2, "the same deferred frame");
    expectTheSamePicture(reference, candidate, "the same deferred frame");

    // TWO EMPTY PICTURES MUST NOT PASS: the reference composite's centre is the ambient-lit surface and it is opaque,
    // so the byte-identical comparison above is comparing two shaded G-buffers rather than two clear colours.
    const std::array<int, 4> centre = pixelAt(reference.pixels, kSide / 2, kSide / 2);
    std::printf("[parity] deferred result centre r=%d g=%d b=%d a=%d\n", centre[0], centre[1], centre[2], centre[3]);
    EXPECT_GT(centre[0] + centre[1] + centre[2], 0)
        << "the reference composite is black: the deferred lighting pass did not shade the G-buffer";
    EXPECT_EQ(centre[3], 255) << "the lighting pass writes an opaque composite";
}

/*
 * @brief The sample limit is ONE NUMBER on both backends, and it is a COUNT.
 *
 * One machine, one GPU: two backends that describe it must agree about how many samples a target may ask for, or a
 * host has to know which backend it is talking to before it can read one limit. A bit set cannot answer this
 * question - the same device used to say "15" - which is what ADR-2026-11-14-sample-count-refused-not-lowered
 * settles. The counting rule lives in each backend's own mapping (they share no library, by design), so the only
 * way it stays one answer is a case that compares the two.
 */
TEST_F(BackendParityTest, TheSampleLimitsAgreeBetweenTheBackends)
{
    const vn::graphics::Capabilities reference = vsg->capabilities();
    const vn::graphics::Capabilities candidate = vk->capabilities();
    std::printf("[parity] max_samples: vsg=%u vk=%u\n", reference.limits.max_samples, candidate.limits.max_samples);

    ASSERT_GT(reference.limits.max_samples, 0U) << "a device always offers one sample";
    EXPECT_EQ(reference.limits.max_samples & (reference.limits.max_samples - 1U), 0U)
        << "Capabilities::limits.max_samples is a sample COUNT, and " << reference.limits.max_samples << " is not a power of two";
    EXPECT_EQ(candidate.limits.max_samples, reference.limits.max_samples)
        << "the two backends describe one GPU with two different sample limits";
}

