/**
 * @brief What a frame RECORDS is what had an EFFECT: the five frame-shape facts that used to live only in the parity suite.
 *
 * @details
 * - These are the five cases the retired parity suite named as having NO standalone successor when the
 *   reference backend retired (`ADR-2026-11-23` D4 step 4): a depth-only clear over a drawn picture, a prepare's shape
 *   against the record, one packet carrying several draws, a pass with no effect, and an entry that names no pass. The
 *   other ten facts that file asserted already have a `test_vk` case of their own (its note lists them), so they are
 *   not repeated here - and its sample-limit case retired with the comparison it was.
 * - WHAT IS ASSERTED IS THE ABSOLUTE EXPECTATION, not a comparison: the reference side is gone, and the numbers below
 *   are the SDK's own rule (see FrameCounts / PrepareReport): a pass or packet that announces nothing is not a scope,
 *   one packet carrying two commands is ONE packet and TWO draws, and the recorded shape equals the asked one.
 * - Frames are hand-built values (`FrameDescription` is the wire); a case that went through the engine would test the
 *   engine twice. Validation is part of the contract: a Warning or Error while recording is a failure here, because
 *   these frames collide several passes into one depth-attached target, which is exactly where an incompatible
 *   recording path shows up first.
 */

#include <gtest/gtest.h>

#include <algorithm>
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
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/Readback.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
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
using vn::graphics::FrameReport;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PrepareReport;
using vn::graphics::PresentOutcome;
using vn::graphics::RenderBackend;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Topology;

namespace
{

/// @brief Side of the picture in pixels: large enough that a 64-texel pass has interior pixels.
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
/// The z values are inside (-1, 1): the identity projection leaves clip z as authored, and a fragment at the far plane
/// would fail every depth comparison.
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle(float z)
{
    std::vector<float> positions{
        -0.75F, -0.75F, z,
        0.75F,  -0.75F, z,
        0.0F,   0.75F,  z,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));

    // Installing positions stated a default non-indexed draw; the triangle is drawn through its index stream, so the
    // default is replaced instead of appended to (see Geometry::addPrimitive).
    vn::graphics::Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = vn::graphics::Primitive::IndexSpan::whole(
        intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
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

/// @brief One collected command: @p geometry through @p program, shaded with @p material.
[[nodiscard]] DrawItem commandOf(const intrusive_ptr<Geometry>& geometry,
                                 const intrusive_ptr<ShaderProgram>& program,
                                 const intrusive_ptr<Material>& material)
{
    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    command.material            = material;
    command.material_description = material->description();
    return command;
}

/// @brief The shared, immutable list a frame's draw packet carries.
[[nodiscard]] std::shared_ptr<const std::vector<DrawItem>> commandsOf(std::vector<DrawItem> commands)
{
    return std::make_shared<const std::vector<DrawItem>>(std::move(commands));
}

/// @brief The draw packet that renders @p commands through the identity camera.
[[nodiscard]] DrawPacket sceneDraw(std::shared_ptr<const std::vector<DrawItem>> commands)
{
    DrawPacket draw;
    draw.kind        = DrawKind::Scene;
    draw.view.camera = identityCamera();
    draw.commands    = std::move(commands);
    return draw;
}

/// @brief A colour target the size every case in this file uses.
[[nodiscard]] intrusive_ptr<RenderTarget> makeColourTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(kSide, kSide);
    return target;
}

/// @brief What one frame answered: its outcome, its record and (when it had a target) its picture.
struct Ran {
    PresentOutcome            present{ PresentOutcome::Skipped };
    FrameReport               report{};
    std::vector<std::uint8_t> pixels{};
    bool                      read{ false };
};

/// @brief Runs @p frame and reads @p target's colour attachment 0 back.
[[nodiscard]] Ran run(RenderBackend& backend, const FrameDescription& frame, const RenderTarget* target)
{
    Ran ran;
    ran.present = backend.executeFrame(frame, nullptr);
    ran.report  = backend.frameReport();
    const ColorReadback read = backend.readColorImage(target, 0);
    ran.pixels               = read.values;
    ran.read                 = read.ok();
    return ran;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback of a `kSide`-square target.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kSide + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

/// @brief Asserts the record a frame must produce: an off-screen present, the asked scopes and no invalid schedule.
void expectRecord(const Ran& ran, std::uint64_t passes, std::uint64_t packets, const char* what)
{
    SCOPED_TRACE(what);
    EXPECT_EQ(ran.present, PresentOutcome::Skipped) << "an off-screen frame presents nowhere";
    ASSERT_TRUE(ran.report.counters.has_value()) << "this backend keeps frame counters";
    EXPECT_EQ(ran.report.counters->passes, passes) << "the pass scopes the frame really entered";
    EXPECT_EQ(ran.report.counters->draw_packets, packets) << "the draw packets that issued something";
    EXPECT_EQ(ran.report.counters->invalid_schedules, 0U);
}

/// @brief Opens a backend for one case (the case skips when the machine has none).
[[nodiscard]] intrusive_ptr<RenderBackend> openBackend()
{
    return RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
}

} // namespace

TEST(VkFrameRecordTest, ADepthOnlyClearKeepsTheDrawnPicture)
{
    // THE HUD LAYER'S CLEAR (see AttachmentPolicy's two dimensions): a later pass into the SAME target loads the colour and
    // resets only the depth - "the marker owns the depth inside its box, the scene underneath stays". What this pins:
    // the picture the first pass drew is still there (the colour was LOADed, not cleared), the depth really was reset
    // (the second pass is recorded with no draws and a clear of its own), and the record counts two scopes and one
    // packet - the second pass has no packet to count.
    auto backend = openBackend();
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> shared = makeColourTarget();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"record_depth_only_clear");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    // Two passes are two identities (a backend keys retained per-pass state by the object), so the frame owns them.
    const intrusive_ptr<RenderPass> painting_identity(new RenderPass());
    const intrusive_ptr<RenderPass> overlay_identity(new RenderPass());

    FrameDescription frame;

    FrameDescription::Pass painting;
    painting.pass  = painting_identity.get();
    painting.order = 0;
    PassDraw paint;
    paint.target      = shared.get();
    paint.attachment_policy = AttachmentPolicy{ Colorf(0.0F, 0.0F, 0.0F, 1.0F), true };
    paint.state.depth = DepthPolicy::TestAndWrite;
    paint.packets.push_back(sceneDraw(commandsOf({ commandOf(geometry, program, red) })));
    painting.draw = std::move(paint);
    frame.passes.push_back(std::move(painting));

    FrameDescription::Pass overlay;
    overlay.pass  = overlay_identity.get();
    overlay.order = 1;
    PassDraw cleaned;
    cleaned.target      = shared.get();
    cleaned.attachment_policy = AttachmentPolicy::depthOnly(); // the colour is LOADed and the depth is CLEARed
    cleaned.state.depth = DepthPolicy::TestAndWrite;
    overlay.draw = std::move(cleaned);
    frame.passes.push_back(std::move(overlay));

    const Ran ran = run(*backend, frame, shared.get());
    expectRecord(ran, 2, 1, "depth-only clear over a drawn picture");
    ASSERT_TRUE(ran.read) << "the target a pass drew into must read back";
    EXPECT_EQ(pixelAt(ran.pixels, kSide / 2, kSide / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the colour was LOADed: the triangle the first pass drew is still the picture";

    backend->shutdown();
}

TEST(VkFrameRecordTest, APrepareReportsTheShapeTheFrameRecords)
{
    // T41: `PrepareReport.passes` / `draw_packets` / `draws` are the WORKLOAD of the frame (see FrameCounts), and a
    // recorded frame's `FrameCounters` follows the same rule. The three passes below are chosen so every half of the
    // rule is exercised at once:
    //   - a CLEAR-ONLY pass: no draw packet, but it clears - a real pass a recorder enters;
    //   - a pass whose only packet issues NOTHING (no camera): it announced neither a draw nor a clear;
    //   - a pass that draws.
    // The asked shape is therefore TWO passes and ONE packet carrying ONE draw, and the record must say the same.
    auto backend = openBackend();
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> shared = makeColourTarget();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"record_prepare_shape");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<AttachmentPolicy> clear = AttachmentPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.25F, 1.0F);

    DrawPacket idle = sceneDraw(commandsOf({ commandOf(geometry, program, red) }));
    idle.view.camera     = CameraSnapshot{}; // present == false: the pass has no view, so it issues no call

    // VALIDATION IS PART OF THE CASE, not a log nobody reads: three passes into one depth-attached target is exactly
    // where a recording path whose pass structures disagree shows up first.
    auto reported = std::make_shared<int>(0);
    backend->setDiagnosticSink([reported](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.severity == vn::graphics::DiagnosticSeverity::Error ||
            diagnostic.severity == vn::graphics::DiagnosticSeverity::Warning) {
            std::printf("[record] severity=%d category=%d %s\n",
                        static_cast<int>(diagnostic.severity),
                        static_cast<int>(diagnostic.category),
                        std::string(reinterpret_cast<const char*>(diagnostic.message.data()), diagnostic.message.size()).c_str());
            ++*reported;
        }
    });

    std::vector<FrameDescription::Pass> pass_entries;
    std::vector<intrusive_ptr<RenderPass>> pass_objects;
    auto                                   add = [&](std::optional<AttachmentPolicy> announced, std::vector<DrawPacket> draws) {
        pass_objects.push_back(intrusive_ptr<RenderPass>(new RenderPass()));
        FrameDescription::Pass entry;
        entry.pass  = pass_objects.back().get();
        entry.order = static_cast<int>(pass_objects.size()) - 1;
        PassDraw pass;
        pass.target      = shared.get();
        pass.attachment_policy = std::move(announced);
        pass.state.depth = DepthPolicy::TestAndWrite;
        pass.packets     = std::move(draws);
        entry.draw       = std::move(pass);
        pass_entries.push_back(std::move(entry));
    };

    add(clear, {});
    add(std::nullopt, { idle });
    add(std::nullopt, { sceneDraw(commandsOf({ commandOf(geometry, program, red) })) });

    FrameDescription frame;
    frame.passes = std::move(pass_entries);

    const PrepareReport asked = backend->prepareFrame(frame, nullptr);
    ASSERT_TRUE(asked.prepared()) << "the backend would not prepare the frame";
    EXPECT_EQ(asked.passes, 2U) << "only the passes that announce something are scopes";
    EXPECT_EQ(asked.draw_packets, 1U) << "only the packet that issues something counts";
    EXPECT_EQ(asked.draws, 1U) << "the one effective packet submits one draw";

    const Ran ran = run(*backend, frame, shared.get());
    expectRecord(ran, 2, 1, "clear + idle + drawing");
    ASSERT_TRUE(ran.report.counters.has_value());
    EXPECT_EQ(ran.report.counters->passes, asked.passes) << "the recorded pass count moved from the asked one";
    EXPECT_EQ(ran.report.counters->draw_packets, asked.draw_packets) << "the recorded packet count moved from the asked one";
    EXPECT_EQ(ran.report.counters->draws, asked.draws) << "the recorded draw count moved from the asked one";
    EXPECT_EQ(*reported, 0) << "the backend reported a problem while preparing and recording the frame";

    backend->shutdown();
}

TEST(VkFrameRecordTest, ADrawPacketCountsPacketsAndDrawsSeparately)
{
    // THE TWO NUMBERS ANSWER DIFFERENT QUESTIONS (see FrameCounts): one draw packet carrying two draws is ONE draw
    // packet and TWO draws - the number a profiler calls draw calls - and the asked counts are the recorded ones.
    auto backend = openBackend();
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> shared = makeColourTarget();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"record_draws");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    std::optional<AttachmentPolicy> clear = AttachmentPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;
    PassDraw pass;
    pass.target      = shared.get();
    pass.attachment_policy = clear;
    pass.state.depth = DepthPolicy::TestAndWrite;
    pass.packets.push_back(
        sceneDraw(commandsOf({ commandOf(geometry, program, red), commandOf(geometry, program, red) })));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));

    const PrepareReport asked = backend->prepareFrame(frame, nullptr);
    ASSERT_TRUE(asked.prepared()) << "the backend would not prepare the frame";
    EXPECT_EQ(asked.draw_packets, 1U) << "one view's list is one draw packet";
    EXPECT_EQ(asked.draws, 2U) << "the packet carries two draws";

    const Ran ran = run(*backend, frame, shared.get());
    expectRecord(ran, 1, 1, "one packet with two draws");
    ASSERT_TRUE(ran.report.counters.has_value());
    EXPECT_EQ(ran.report.counters->draw_packets, asked.draw_packets) << "the recorded packet count moved from the asked one";
    EXPECT_EQ(ran.report.counters->draws, asked.draws) << "the recorded draw count moved from the asked one";

    backend->shutdown();
}

TEST(VkFrameRecordTest, APassWithNoEffectIsNotAScope)
{
    // A RECORD IS WHAT HAD AN EFFECT (see FrameCounters): an entry that announces neither a clear nor a draw packet
    // records nothing - and the target it never named was never built, so there is no picture to answer with.
    auto backend = openBackend();
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> shared = makeColourTarget();

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;
    frame.passes.push_back(std::move(entry));

    const Ran ran = run(*backend, frame, shared.get());
    expectRecord(ran, 0, 0, "pass with no effect");
    EXPECT_FALSE(ran.read) << "a pass that announces no target cannot have produced a picture";

    backend->shutdown();
}

TEST(VkFrameRecordTest, AnEntryThatNamesNoPassIsNothingToRun)
{
    // THE SDK'S RULE (see RenderFrameExecutor::executeFrame): an entry with a null pass is NOTHING TO RUN. A frame
    // that would otherwise draw must do nothing at all - no pass scope, no draw packet, no picture.
    auto backend = openBackend();
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> shared = makeColourTarget();

    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFragmentStage, u8"record_no_pass");
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass  = nullptr;
    entry.order = 0;
    PassDraw pass;
    pass.target      = shared.get();
    std::optional<AttachmentPolicy> clear = AttachmentPolicy{};
    clear->color                     = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.attachment_policy                = clear;
    pass.state.depth = DepthPolicy::TestAndWrite;
    pass.packets.push_back(sceneDraw(commandsOf({ commandOf(geometry, program, red) })));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));

    const Ran ran = run(*backend, frame, shared.get());
    expectRecord(ran, 0, 0, "entry without a pass");
    EXPECT_FALSE(ran.read) << "an entry that names no pass must not have drawn anything";

    backend->shutdown();
}
