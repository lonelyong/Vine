/**
 * @brief M7: ONE FrameDescription, drawn by both backends - the same record and the same picture.
 *
 * WHY THIS FILE LIVES IN THE REFERENCE BACKEND'S SUITE. The parity claim is "the seam is enough": a frame value
 * that the reference backend draws must be drawn by a second, independently written backend with the same record
 * and the same pixels (`.ai/research/graphics-2026-11-13-gpu-resource-abstraction.md` §8.3 M7, §9 D4). The
 * reference half is the vsg device this suite already brings up for its pixel cases; the vk half is the same
 * backend `test_vk` exercises, compiled into this binary. Two backends in ONE PROCESS is the point - a diff
 * between two processes would compare files, not answers.
 *
 * WHAT IS COMPARED, AND WHAT IS NOT.
 * - THE RECORD: `FrameReport::counters` (pass scopes, drawing calls) and `PresentOutcome`, for the same call.
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
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameStats.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ImageSource.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
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
using vn::graphics::DepthMode;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::FrameReport;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PassRenderState;
using vn::graphics::PresentOutcome;
using vn::graphics::RenderBackend;
using vn::graphics::RenderCommand;
using vn::graphics::RenderState;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;
using vn::intrusive_ptr;

namespace
{

/// @brief Side of the picture in pixels: the size the reference pixel cases use, so a diff covers the frame.
constexpr int kSide = 64;

/// @brief The content ABI's camera push: `projection` then `modelView`, both mat4 (see ShaderAbi / ContentPush).
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
[[nodiscard]] RenderCommand commandOf(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<ShaderProgram>& program, const intrusive_ptr<Material>& material)
{
    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    if (material != nullptr) {
        command.material             = material;
        command.material_description = material->description();
    }
    return command;
}

/// @brief The shared, immutable list a frame's drawing call carries.
[[nodiscard]] std::shared_ptr<const std::vector<RenderCommand>> commandsOf(std::vector<RenderCommand> commands)
{
    return std::make_shared<const std::vector<RenderCommand>>(std::move(commands));
}

/// @brief The drawing call that renders @p commands through the identity camera.
[[nodiscard]] DrawDescription sceneDraw(std::shared_ptr<const std::vector<RenderCommand>> commands)
{
    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = std::move(commands);
    return draw;
}

/// @brief A frame with ONE pass: it names @p target, optionally clears it, and issues @p draws.
///
/// @p inputs is what the pass DECLARES to sample (a produced target for a screen draw, an imported texture for a
/// material map): the reference backend binds its sampler from that declaration, so a frame that samples an image
/// without declaring it is incomplete - the same rule the engine follows when it fills FrameDescription::Pass.
[[nodiscard]] FrameDescription onePass(RenderTarget* target,
                                       std::optional<ClearPolicy> clear,
                                       DepthMode depth,
                                       std::vector<DrawDescription> draws,
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
    pass.draws        = std::move(draws);
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
void expectTheSameRecord(const Ran& reference, const Ran& candidate, std::uint64_t passes, std::uint64_t draws, const char* what)
{
    SCOPED_TRACE(what);
    EXPECT_EQ(reference.present, PresentOutcome::Skipped) << "an off-screen frame presents nowhere";
    EXPECT_EQ(candidate.present, reference.present) << "the same frame is answered the same way";

    ASSERT_TRUE(reference.report.counters.has_value()) << "the reference backend keeps frame counters";
    ASSERT_TRUE(candidate.report.counters.has_value()) << "this backend keeps frame counters";
    EXPECT_EQ(reference.report.counters->passes, passes) << "the reference backend's pass scopes";
    EXPECT_EQ(candidate.report.counters->passes, reference.report.counters->passes)
        << "the same frame is the same number of pass scopes";
    EXPECT_EQ(reference.report.counters->draws, draws) << "the reference backend's drawing calls";
    EXPECT_EQ(candidate.report.counters->draws, reference.report.counters->draws)
        << "the same frame is the same number of drawing calls";
    EXPECT_EQ(candidate.report.counters->invalid_schedules, 0U);
    std::printf("[parity] %s: passes=%llu draws=%llu (both backends)\n",
                what,
                static_cast<unsigned long long>(candidate.report.counters->passes),
                static_cast<unsigned long long>(candidate.report.counters->draws));
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

    // Declaration order matters: the devices must die BEFORE the targets they hold (members destroy in reverse).
    std::vector<intrusive_ptr<RenderTarget>> targets;
    intrusive_ptr<RenderBackend>             vsg;
    intrusive_ptr<RenderBackend>             vk;
};

TEST_F(BackendParityTest, TheSameClearIsTheSamePicture)
{
    // THE SMALLEST FRAME: one pass, one clear, no drawing call. What it pins is that both backends clear to the
    // SAME stored bytes - the state every later case starts from.
    RenderTarget* const shared = target();

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.25F, 0.0F, 1.0F);

    const FrameDescription frame = onePass(shared, clear, DepthMode::TestAndWrite, {});

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
        onePass(shared, clear, DepthMode::TestAndWrite, { sceneDraw(commandsOf({ commandOf(geometry, program, red) })) });

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 1, 1, "opaque triangle");
    expectTheSamePicture(reference, candidate, "opaque triangle");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the reference picture is not the fragment stage's colour";
    EXPECT_EQ(pixelAt(reference.pixels, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 })) << "the reference picture is not the pass' clear";
    EXPECT_EQ(pixelAt(candidate.pixels, kSide / 2, kSide / 2), pixelAt(reference.pixels, kSide / 2, kSide / 2));
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
                                               DepthMode::TestAndWrite,
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

    RenderCommand over = commandOf(geometry, program, green);
    over.opacity       = 0.5F;
    over.authored_state.blend =
        BlendState{ true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha };

    std::optional<ClearPolicy> clear = ClearPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription blending =
        onePass(shared, clear, DepthMode::Disabled, { sceneDraw(commandsOf({ commandOf(geometry, program, red), over })) });

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

TEST_F(BackendParityTest, TheSameScreenCopySamplesTheSameColours)
{
    // THE FULL-SCREEN PATH: a scene frame fills the source, a screen frame samples it into the destination. Two
    // frames, each compared as a record and as pixels - the copy must reproduce the drawn centre AND the cleared
    // corner, which is what tells "sampled the source" from "cleared the destination".
    RenderTarget* const source      = target();
    RenderTarget* const destination = target();

    const intrusive_ptr<ShaderProgram> scene  = makeProgramWith(kFragmentStage, u8"parity_screen_source");
    const intrusive_ptr<ShaderProgram> copy   = vn::graphics::screenCopyProgram(0);
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<ClearPolicy> source_clear = ClearPolicy{};
    source_clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    const FrameDescription scene_frame =
        onePass(source, source_clear, DepthMode::TestAndWrite, { sceneDraw(commandsOf({ commandOf(geometry, scene, red) })) });

    DrawDescription screen;
    screen.kind                = DrawDescription::Kind::Screen;
    screen.camera              = identityCamera();
    screen.source              = source;
    screen.program             = copy;
    screen.program_description = copy->description();
    const FrameDescription screen_frame =
        onePass(destination, std::nullopt, DepthMode::Disabled, { screen }, { vn::graphics::ImageSource::producedTarget(source) });

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
                                               DepthMode::TestAndWrite,
                                               { sceneDraw(commandsOf({ commandOf(geometry, program, material) })) },
                                               { vn::graphics::ImageSource::imported(texture.get()) });

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 1, 1, "sampled texture");
    expectTheSamePicture(reference, candidate, "sampled texture");
    EXPECT_EQ(pixelAt(reference.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 0, 0, 255, 255 }))
        << "the reference backend did not sample the base-colour texture";
}

TEST_F(BackendParityTest, APassWithNoEffectIsNotAScopeOnEitherBackend)
{
    // A RECORD IS WHAT HAD AN EFFECT (see FrameCounters / the reference recorder): an entry that announces neither a
    // clear nor a drawing call records nothing on either backend - and the target it never named was never built, so
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
    // that would otherwise draw must do nothing at all - no pass scope, no drawing call, no picture.
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
    pass.state.depth  = DepthMode::TestAndWrite;
    pass.draws.push_back(sceneDraw(commandsOf({ commandOf(geometry, program, red) })));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));

    const Ran reference = run(*vsg, frame, shared);
    const Ran candidate = run(*vk, frame, shared);

    expectTheSameRecord(reference, candidate, 0, 0, "entry without a pass");
    EXPECT_FALSE(reference.read) << "an entry that names no pass must not have drawn anything";
    EXPECT_EQ(candidate.read, reference.read) << "the same frame is refused the same way";
}
