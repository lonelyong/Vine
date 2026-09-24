/**
 * @brief Device-free tests of the frame's sweep: what the host let go is let go, once per frame, in one call.
 *
 * WHY THIS SUITE EXISTS. Both halves of the sweep - ContentStore::releaseAbandoned and
 * MaterialImages::releaseAbandoned - were implemented and unit-tested in their own suites, and NEITHER was
 * called by the frame drive: a host that dropped a geometry, a program, a material or a texture kept its
 * tables, its halves, its pipelines and its images for the whole session (the defect this unit was added to
 * close). A per-half test cannot see that: each half was correct. What was missing is the COMPOSITION - one
 * call, in the frame's order, that covers both and reports what it let go.
 *
 * So these cases assert the composition: one call covers both caches; live content is never let go; an
 * abandoned object's rows stay answerable until the park comes due (the rule that makes dropping them safe);
 * and a second sweep does not count the same object twice.
 */

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>

#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/ContentSweep.hpp>
#include <vine/vsg/api/ProgramVariant.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>

using vine::graphics::Geometry;
using vine::graphics::Material;
using vine::graphics::ShaderProgram;
using vine::graphics::ShaderStage;
using vine::graphics::ShaderStageType;
using vine::graphics::Texture2D;
using vine::imaging::Image;
using vine::imaging::PixelFormat;
using vine::vsg::ContentFacts;
using vine::vsg::ContentStore;
using vine::vsg::detail::TextureReject;
using vine::vsg::MaterialImages;
using vine::vsg::SweepOutcome;
using vine::vsg::releaseAbandonedContent;
using vine::vsg::findGeometry;
using vine::vsg::findMaterial;
using vine::vsg::findProgram;
using vine::vsg::core::CompiledCommand;
using vine::vsg::core::CompiledDraw;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::CompiledPass;
using vine::vsg::core::DrawKind;
using vine::vsg::core::FrameTimeline;
using vine::vsg::core::ProgramRef;
using vine::vsg::core::RetirementQueue;

namespace
{

/// @brief A stage of @p type whose source is @p source (the ASCII form the SDK stores as UTF-8).
ShaderStage stage(ShaderStageType type, const char* source)
{
    ShaderStage out;
    out.type       = type;
    out.source     = vine::String(reinterpret_cast<const char8_t*>(source));
    out.entryPoint = vine::String(reinterpret_cast<const char8_t*>("main"));
    return out;
}

/// @brief A content program: the smallest pair of stages a content pipeline can be built from.
vine::intrusive_ptr<ShaderProgram> contentProgram()
{
    const auto program = vine::intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->addStage(stage(ShaderStageType::Vertex,
                            "#version 450\n"
                            "layout(location = 0) in vec3 position;\n"
                            "void main() { gl_Position = vec4(position, 1.0); }\n"));
    program->addStage(stage(ShaderStageType::Fragment,
                            "#version 450\n"
                            "layout(location = 0) out vec4 color;\n"
                            "void main() { color = vec4(1.0); }\n"));
    return program;
}

/// @brief A textured quad, with every stream filled (so the store describes it without refusing).
vine::intrusive_ptr<Geometry> quad()
{
    const auto geometry = vine::intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(
        std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F })));
    geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);
    return geometry;
}

/// @brief A material that samples @p texture when one is given.
vine::intrusive_ptr<Material> material(vine::intrusive_ptr<Texture2D> texture = {})
{
    const auto out = vine::intrusive_ptr<Material>(new Material());
    out->setDiffuse(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));
    if (texture != nullptr)
    {
        out->setTexture(texture);
    }
    return out;
}

/// @brief A filled, single-level 2D texture: the shape the image cache accepts.
vine::intrusive_ptr<Texture2D> readyTexture()
{
    auto texture = vine::intrusive_ptr<Texture2D>(new Texture2D(4, 4, PixelFormat::Rgba8Unorm));
    texture->setImage(vine::intrusive_ptr<const Image>(new Image(4, 4, PixelFormat::Rgba8Unorm)));
    return texture;
}

/// @brief One frame's plan, built by hand: the store walks identities, and a hand-built plan has them.
struct Plan
{
    std::vector<CompiledCommand> commands;
    std::vector<CompiledDraw>    draws;
    std::vector<CompiledPass>    passes;
    CompiledFrame                frame{};
};

/// @brief A plan with one pass and one content draw naming @p geometry, @p program and @p material.
std::unique_ptr<Plan> contentPlan(const void* geometry, std::uint64_t geometry_revision, const void* program,
                                  const void* material)
{
    auto plan = std::make_unique<Plan>();

    CompiledCommand command;
    command.geometry          = geometry;
    command.geometry_revision = geometry_revision;
    command.program           = ProgramRef{ program, 0U };
    command.material          = material;
    plan->commands.push_back(command);

    CompiledDraw draw;
    draw.kind     = DrawKind::Content;
    draw.commands = plan->commands;
    plan->draws.push_back(draw);

    CompiledPass pass;
    pass.draws = plan->draws;
    plan->passes.push_back(pass);

    plan->frame.passes          = plan->passes;
    plan->frame.default_program = ProgramRef{ program, 0U };
    return plan;
}

/// @brief Drives the timeline past @p frame and runs the queue's releases (the frame's last step).
void releaseUpTo(FrameTimeline& timeline, RetirementQueue& retirement, std::uint64_t frame)
{
    timeline.completeUpTo(frame);
    retirement.advance(timeline);
}

}  // namespace

TEST(ContentSweepTest, NothingIsLetGoWhileTheHostStillHoldsEverything)
{
    ContentStore                            store;
    const std::shared_ptr<MaterialImages>    cache = MaterialImages::create();
    FrameTimeline                           timeline;
    RetirementQueue                         retirement(1U);
    ASSERT_NE(cache, nullptr);

    const auto geometry = quad();
    const auto program  = contentProgram();
    const auto texture  = readyTexture();
    const auto shading  = material(texture);

    store.track(geometry);
    store.track(program);
    store.track(shading);

    const auto plan = contentPlan(geometry.get(), geometry->revision(), program.get(), shading.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);
    ASSERT_EQ(store.programEntries(), 1U);
    ASSERT_EQ(store.materialEntries(), 1U);

    TextureReject reason = TextureReject::Ok;
    (void)cache->acquire(texture.get(), reason);
    ASSERT_EQ(cache->count(), 1U);
    ASSERT_EQ(reason, TextureReject::Ok);

    // The host holds every one of them: the sweep has nothing to do, and nothing to park.
    const SweepOutcome outcome = releaseAbandonedContent(store, *cache, timeline, retirement);
    EXPECT_EQ(outcome.objects, 0U) << "a geometry, a program and a material the host still holds";
    EXPECT_EQ(outcome.textures, 0U) << "a texture the host still holds (through its material)";
    EXPECT_EQ(store.geometryEntries(), 1U);
    EXPECT_EQ(store.programEntries(), 1U);
    EXPECT_EQ(store.materialEntries(), 1U);
    EXPECT_EQ(cache->count(), 1U);
    EXPECT_EQ(retirement.pending(), 0U) << "nothing was abandoned, so nothing was parked";
}

TEST(ContentSweepTest, EverythingTheHostDroppedIsLetGoInOneCallAndItsRowsLeaveAtThePark)
{
    ContentStore                          store;
    const std::shared_ptr<MaterialImages>  cache = MaterialImages::create();
    FrameTimeline                         timeline;
    RetirementQueue                       retirement(1U);
    ASSERT_NE(cache, nullptr);

    auto geometry = quad();
    auto program  = contentProgram();
    auto texture  = readyTexture();
    auto shading  = material(texture);

    store.track(geometry);
    store.track(program);
    store.track(shading);

    const auto          plan  = contentPlan(geometry.get(), geometry->revision(), program.get(), shading.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);

    TextureReject reason = TextureReject::Ok;
    (void)cache->acquire(texture.get(), reason);
    ASSERT_EQ(cache->count(), 1U);

    // The identities the rows are keyed by: what a later lookup would use.
    const void* const   geometry_address  = geometry.get();
    const std::uint64_t geometry_revision = geometry->revision();

    // The host lets go of the whole drawable - the texture goes with the material that held it.
    const vine::graphics::Texture* texture_address = texture.get();
    shading.reset();
    geometry.reset();
    program.reset();
    texture.reset();

    const SweepOutcome outcome = releaseAbandonedContent(store, *cache, timeline, retirement);
    EXPECT_EQ(outcome.objects, 3U) << "the geometry, the program and the material the host dropped";
    EXPECT_EQ(outcome.textures, 1U) << "the texture nothing sampled any more";
    EXPECT_FALSE(cache->has(texture_address)) << "the image cache has no entry left to look up";

    // The ROWS stay answerable until the park comes due: a plan recorded in the frame that is being finished
    // may still name them (see api/ContentSweep), which is why this half parks instead of erasing.
    EXPECT_GE(retirement.pending(), 1U);
    EXPECT_TRUE(findGeometry(facts, geometry_address, geometry_revision).found())
        << "the row of the dropped geometry still answers while its park is open";
    EXPECT_EQ(store.geometryEntries(), 1U);

    // And once the slots that could still name them are past, they leave.
    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.geometryEntries(), 0U);
    EXPECT_EQ(store.programEntries(), 0U);
    EXPECT_EQ(store.materialEntries(), 0U);
}

TEST(ContentSweepTest, AnObjectIsLetGoOnceAndASecondSweepCountsNothing)
{
    ContentStore                          store;
    const std::shared_ptr<MaterialImages>  cache = MaterialImages::create();
    FrameTimeline                         timeline;
    RetirementQueue                       retirement(1U);
    ASSERT_NE(cache, nullptr);

    auto geometry = quad();
    auto program  = contentProgram();

    store.track(geometry);
    store.track(program);

    const auto plan = contentPlan(geometry.get(), geometry->revision(), program.get(), nullptr);
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);

    geometry.reset();
    program.reset();

    const SweepOutcome first = releaseAbandonedContent(store, *cache, timeline, retirement);
    EXPECT_EQ(first.objects, 2U);

    // The live set no longer holds them, so the second sweep has nothing to forget: a sweep that counted the
    // same object every frame would make "nothing is retained" unobservable.
    const SweepOutcome second = releaseAbandonedContent(store, *cache, timeline, retirement);
    EXPECT_EQ(second.objects, 0U);
    EXPECT_EQ(second.textures, 0U);
}

TEST(ContentSweepTest, TheDefaultMaterialIsNeverAbandoned)
{
    ContentStore                          store;
    const std::shared_ptr<MaterialImages>  cache = MaterialImages::create();
    FrameTimeline                         timeline;
    RetirementQueue                       retirement(1U);
    ASSERT_NE(cache, nullptr);

    auto geometry = quad();
    auto program  = contentProgram();

    store.track(geometry);
    store.track(program);

    // Content with no material of its own: the plan names nullptr, and the store answers with its own
    // default entry, which no host handle can abandon (see ContentStore::releaseAbandoned).
    const auto plan = contentPlan(geometry.get(), geometry->revision(), program.get(), nullptr);
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.materialEntries(), 1U);

    geometry.reset();
    program.reset();

    const SweepOutcome outcome = releaseAbandonedContent(store, *cache, timeline, retirement);
    EXPECT_EQ(outcome.objects, 2U) << "the geometry and the program, never the default material";
    EXPECT_EQ(store.materialEntries(), 1U) << "the default entry stays: it is not an object anyone holds";
}
