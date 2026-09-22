#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/intrusive_ptr.hpp>

#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentStore.hpp>
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
using vine::vsg::ContentFacts;
using vine::vsg::ContentStore;
using vine::vsg::FactMiss;
using vine::vsg::findGeometry;
using vine::vsg::findMaterial;
using vine::vsg::findProgram;
using vine::vsg::GeometryFacts;
using vine::vsg::MaterialFacts;
using vine::vsg::ProgramVariant;
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

/// @brief A stage of @p type, with the given source (the ASCII form the SDK stores as UTF-8).
ShaderStage stage(ShaderStageType type, const char* source)
{
    ShaderStage out;
    out.type       = type;
    out.source     = vine::String(reinterpret_cast<const char8_t*>(source));
    out.entryPoint = vine::String(reinterpret_cast<const char8_t*>("main"));
    return out;
}

/// @brief A content program whose fragment stage gates its sampler on `VINE_DIFFUSE_MAP`: one text, two ABIs.
constexpr const char* kGatedVertexSource =
    "#version 450\n"
    "#pragma import_defines (VINE_DIFFUSE_MAP)\n"
    "layout(location = 0) in vec3 position;\n"
    "#ifdef VINE_DIFFUSE_MAP\n"
    "layout(location = 8) in vec2 texcoord;\n"
    "#endif\n"
    "void main() { gl_Position = vec4(position, 1.0); }\n";

constexpr const char* kGatedFragmentSource =
    "#version 450\n"
    "#pragma import_defines (VINE_DIFFUSE_MAP)\n"
    "layout(location = 0) out vec4 color;\n"
    "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
    "{\n"
    "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
    "} material;\n"
    "#ifdef VINE_DIFFUSE_MAP\n"
    "layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
    "#endif\n"
    "void main() { color = material.diffuse; }\n";

constexpr const char* kScreenFragmentSource =
    "#version 450\n"
    "layout(location = 0) in vec2 vine_uv;\n"
    "layout(location = 0) out vec4 color;\n"
    "void main() { color = vec4(vine_uv, 0.0, 1.0); }\n";

/// @brief A content program with both stages (the identity the plan names).
vine::intrusive_ptr<ShaderProgram> contentProgram()
{
    const auto program = vine::intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->addStage(stage(ShaderStageType::Vertex, kGatedVertexSource));
    program->addStage(stage(ShaderStageType::Fragment, kGatedFragmentSource));
    return program;
}

/// @brief A full-screen program: a fragment stage only (the engine brings the vertex stage).
vine::intrusive_ptr<ShaderProgram> screenProgram()
{
    const auto program = vine::intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->addStage(stage(ShaderStageType::Fragment, kScreenFragmentSource));
    return program;
}

/// @brief A quad, with the texcoord channel when @p texcoords asks for one.
vine::intrusive_ptr<Geometry> quad(bool texcoords = false)
{
    const auto geometry = vine::intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(
        std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F })));
    if (texcoords)
    {
        geometry->setTexcoords2(vine::intrusive_ptr<const vine::Buffer<float>>(
            new vine::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F })));
    }
    geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);
    return geometry;
}

/// @brief A material with a diffuse colour, and (when @p texture is not null) a texture to sample.
vine::intrusive_ptr<Material> material(vine::Colorf diffuse,
                                       vine::intrusive_ptr<Texture2D> texture = {})
{
    const auto out = vine::intrusive_ptr<Material>(new Material());
    out->setDiffuse(diffuse);
    if (texture != nullptr)
    {
        out->setTexture(texture);
    }
    return out;
}

/// @brief Reads a material entry's block back as the engine's block (the bytes ARE the shading's input).
vine::graphics::VineMaterialBlock blockOf(const MaterialFacts& facts)
{
    vine::graphics::VineMaterialBlock block{};
    if (facts.block.size() == sizeof(block))
    {
        std::memcpy(&block, facts.block.data(), sizeof(block));
    }
    return block;
}

/// @brief One frame's plan, built by hand: the store walks identities, and a hand-built plan is the
/// smallest thing that has them.
struct Plan
{
    std::vector<CompiledCommand> commands;
    std::vector<CompiledDraw>    draws;
    std::vector<CompiledPass>    passes;
    CompiledFrame                frame{};
};

/// @brief A content command naming @p geometry (at @p geometry_revision), @p program and @p material.
CompiledCommand command(const void* geometry, std::uint64_t geometry_revision, const void* program,
                        const void* material)
{
    CompiledCommand out;
    out.geometry          = geometry;
    out.geometry_revision = geometry_revision;
    out.program           = ProgramRef{ program, 0U };
    out.material          = material;
    return out;
}

/// @brief A plan with one pass and one content draw over @p commands.
std::unique_ptr<Plan> contentPlan(std::vector<CompiledCommand> commands, const ShaderProgram* program = nullptr)
{
    auto plan     = std::make_unique<Plan>();
    plan->commands = std::move(commands);

    CompiledDraw draw;
    draw.kind     = DrawKind::Content;
    draw.commands = plan->commands;
    plan->draws.push_back(draw);

    CompiledPass pass;
    pass.draws = plan->draws;
    plan->passes.push_back(pass);
    plan->frame.passes = plan->passes;
    if (program != nullptr)
    {
        plan->frame.default_program = ProgramRef{ program, 0U };
    }
    return plan;
}

/// @brief A plan with one pass and one full-screen draw of @p program.
std::unique_ptr<Plan> screenPlan(const ShaderProgram* program)
{
    auto plan = std::make_unique<Plan>();

    CompiledDraw draw;
    draw.kind    = DrawKind::Screen;
    draw.program = ProgramRef{ program, 0U };
    plan->draws.push_back(draw);

    CompiledPass pass;
    pass.draws = plan->draws;
    plan->passes.push_back(pass);
    plan->frame.passes = plan->passes;
    return plan;
}

/// @brief Drives the timeline past @p frame and runs the queue's releases.
void releaseUpTo(FrameTimeline& timeline, RetirementQueue& retirement, std::uint64_t frame)
{
    timeline.completeUpTo(frame);
    retirement.advance(timeline);
}

}  // namespace

TEST(ContentStoreTest, ThePlanDecidesWhatIsBuilt)
{
    ContentStore store;
    const auto   named    = quad();
    const auto   unnamed  = quad();
    const auto   program  = contentProgram();
    const auto   material = ::material(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    store.track(named);
    store.track(unnamed);
    store.track(program);
    store.track(material);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(named.get(), named->revision(), program.get(), material.get()) },
                                  program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    EXPECT_EQ(store.geometryEntries(), 1U) << "the geometry no plan names is not built";
    EXPECT_EQ(store.programEntries(), 1U);
    EXPECT_EQ(store.materialEntries(), 1U);
    EXPECT_EQ(store.builds(), 3U);

    EXPECT_TRUE(findGeometry(facts, named.get(), named->revision()).found());
    EXPECT_TRUE(findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{}).found());
    EXPECT_TRUE(findMaterial(facts, material.get()).found());

    // A second frame naming the other three builds the other three, and nothing else.
    const auto other_plan =
        contentPlan({ command(unnamed.get(), unnamed->revision(), program.get(), nullptr) }, program.get());
    const ContentFacts& second = store.tablesFor(other_plan->frame, timeline, retirement);
    EXPECT_EQ(store.geometryEntries(), 2U);
    EXPECT_EQ(store.programEntries(), 1U);
    EXPECT_EQ(store.materialEntries(), 2U) << "the default material joins the table the first time content without one is named";
    EXPECT_EQ(store.builds(), 5U) << "the other geometry and the default material, and nothing else (the program is already answered)";
    EXPECT_TRUE(findMaterial(second, nullptr).found());
}

TEST(ContentStoreTest, ASteadyFrameBuildsNothing)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    const auto   material = ::material(vine::Colorf(0.1F, 0.2F, 0.3F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(material);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto          plan  = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) },
                                            program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);
    const std::uint64_t built = store.builds();

    for (int frame = 0; frame < 3; ++frame)
    {
        ASSERT_TRUE(store.tablesFor(plan->frame, timeline, retirement).programs.data() == facts.programs.data())
            << "the tables are the same storage each frame";
        EXPECT_EQ(store.builds(), built) << "a steady frame builds nothing";
    }
    EXPECT_EQ(retirement.pending(), 0U) << "nothing was superseded, so nothing was parked";
}

TEST(ContentStoreTest, ARevisionBumpJoinsTheTablesAndTheOldRevisionLeavesAtItsPark)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();

    store.track(geometry);
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) },
                                  program.get());
    const ContentFacts& first = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_TRUE(findGeometry(first, geometry.get(), geometry->revision()).found());
    const std::uint64_t old_revision = geometry->revision();

    // The host edits the data and announces it (the SDK's contract): the plan still names the old revision.
    geometry->bumpRevision();
    const std::uint64_t new_revision = geometry->revision();
    EXPECT_NE(new_revision, old_revision);

    const ContentFacts& second = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.geometryEntries(), 2U) << "the new revision JOINS the table, it does not replace it";
    EXPECT_TRUE(findGeometry(second, geometry.get(), new_revision).found()) << "the live revision answers";
    EXPECT_TRUE(findGeometry(second, geometry.get(), old_revision).found())
        << "and so does the revision a recorded frame may still name";
    EXPECT_EQ(retirement.pending(), 1U) << "the supersession was parked";

    // Not yet due: the park's window is the slots that could still record the old revision.
    releaseUpTo(timeline, retirement, 1U);
    EXPECT_EQ(retirement.pending(), 1U) << "released a frame early";
    EXPECT_TRUE(findGeometry(second, geometry.get(), old_revision).found());

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(retirement.pending(), 0U);
    EXPECT_EQ(store.geometryEntries(), 1U) << "the superseded revision left the table";
    EXPECT_EQ(findGeometry(store.tablesFor(plan->frame, timeline, retirement), geometry.get(), old_revision).miss,
              FactMiss::Revision)
        << "a plan naming it now misses with Revision (the identity is known, that revision is not)";
}

TEST(ContentStoreTest, AMaterialEditReplacesItsEntryAndParksTheValue)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    const auto   material = ::material(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(material);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) },
                                  program.get());
    const ContentFacts& first = store.tablesFor(plan->frame, timeline, retirement);
    const auto           first_block = blockOf(*findMaterial(first, material.get()).entry);
    EXPECT_FLOAT_EQ(first_block.diffuse[0], 0.25F);

    // The host edits the material and reports it (the SDK MaterialManager's contract).
    material->setDiffuse(vine::Colorf(0.75F, 0.5F, 0.25F, 1.0F));
    store.updateMaterial(material.get());

    const ContentFacts& second = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.materialEntries(), 1U) << "a material is answered by identity alone: the row is replaced";
    const auto second_block = blockOf(*findMaterial(second, material.get()).entry);
    EXPECT_FLOAT_EQ(second_block.diffuse[0], 0.75F) << "the table answers what the material is NOW";
    EXPECT_EQ(retirement.pending(), 1U) << "the replaced value was parked";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(retirement.pending(), 0U);

    // An edit for a material nobody tracked is answered with nothing at all (it cannot be drawn either).
    const auto untracked = ::material(vine::Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    store.updateMaterial(untracked.get());
    EXPECT_EQ(store.materialEntries(), 1U);
}

TEST(ContentStoreTest, AProgramIsOneEntryPerVariant)
{
    ContentStore store;
    const auto   geometry   = quad(true);  // the texcoord channel is what a cube variant would widen
    const auto   program    = contentProgram();
    const auto   texture    = vine::intrusive_ptr<Texture2D>(new Texture2D(1, 1, vine::imaging::PixelFormat::Rgba8Unorm));
    const auto   textured   = ::material(vine::Colorf(1.0F, 1.0F, 1.0F, 1.0F), texture);
    const auto   plain      = ::material(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(textured);
    store.track(plain);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), textured.get()),
                                    command(geometry.get(), geometry->revision(), program.get(), plain.get()) },
                                  program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    ASSERT_EQ(store.programEntries(), 2U) << "one program, two texts: two entries";

    ProgramVariant textured_variant;
    textured_variant.diffuse_map = true;
    const ProgramVariant plain_variant;

    const auto textured_entry = findProgram(facts, ProgramRef{ program.get(), program->revision() }, textured_variant);
    const auto plain_entry    = findProgram(facts, ProgramRef{ program.get(), program->revision() }, plain_variant);
    ASSERT_TRUE(textured_entry.found());
    ASSERT_TRUE(plain_entry.found());
    EXPECT_EQ(textured_entry.entry->abi.bindings.size(), plain_entry.entry->abi.bindings.size() + 1U)
        << "the sampler is what the material's texture adds";
    EXPECT_NE(textured_entry.entry->shaders.defines, plain_entry.entry->shaders.defines);

    // The variant the store built each entry for is the one the DRAWABLE asked for (see api/ProgramVariant).
    EXPECT_EQ(textured_entry.entry->variant, textured_variant);
    EXPECT_EQ(plain_entry.entry->variant, plain_variant);
}

TEST(ContentStoreTest, AScreenProgramGetsTheEnginesOwnVertexStage)
{
    ContentStore store;
    const auto   program = screenProgram();
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto          plan  = screenPlan(program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    ASSERT_EQ(store.programEntries(), 1U);
    const auto entry = findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{});
    ASSERT_TRUE(entry.found());
    EXPECT_NE(entry.entry->shaders.vertex.find("gl_VertexIndex"), std::string::npos)
        << "the full-screen ABI's vertex stage is the engine's triangle, not the host's";
}

TEST(ContentStoreTest, AnUntrackedObjectIsSimplyAbsent)
{
    ContentStore store;

    // The objects exist and the plan names them, but the host never handed them over: a store cannot build
    // facts from an address alone, and it does not pretend to.
    const auto geometry = quad();
    const auto program  = contentProgram();
    const auto material = ::material(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) },
                                  program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    EXPECT_EQ(store.geometryEntries(), 0U);
    EXPECT_EQ(store.programEntries(), 0U);
    EXPECT_EQ(store.materialEntries(), 0U);
    EXPECT_EQ(store.builds(), 0U);
    EXPECT_EQ(findGeometry(facts, geometry.get(), geometry->revision()).miss, FactMiss::Unknown);
    EXPECT_EQ(findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{}).miss, FactMiss::Unknown);
    EXPECT_EQ(findMaterial(facts, material.get()).miss, FactMiss::Unknown);
}

TEST(ContentStoreTest, AnObjectNobodyElseHoldsIsReleasedAndItsRowsLeaveAtThePark)
{
    ContentStore store;
    auto         geometry = quad();
    auto         program  = contentProgram();

    store.track(geometry);
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) },
                                  program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);
    ASSERT_EQ(store.programEntries(), 1U);

    // The host still holds both, so neither is abandoned.
    EXPECT_EQ(store.releaseAbandoned(timeline, retirement), 0U);
    EXPECT_EQ(store.geometryEntries(), 1U);

    // The host lets the geometry go: the store's share is the last one.
    const void* const identity = geometry.get();
    const std::uint64_t revision = geometry->revision();
    geometry.reset();
    EXPECT_EQ(store.releaseAbandoned(timeline, retirement), 1U);
    EXPECT_EQ(retirement.pending(), 1U) << "the abandoned rows are parked, not dropped: a recorded frame may still name them";
    EXPECT_TRUE(findGeometry(store.tablesFor(plan->frame, timeline, retirement), identity, revision).found())
        << "the rows still answer while the parking window is open";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.geometryEntries(), 0U);
    EXPECT_EQ(store.programEntries(), 1U) << "the program is still held by the host";
}
