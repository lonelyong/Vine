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

using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::vsg::ContentFacts;
using vn::vsg::ContentStore;
using vn::vsg::FactMiss;
using vn::vsg::findGeometry;
using vn::vsg::findMaterial;
using vn::vsg::findProgram;
using vn::vsg::GeometryFacts;
using vn::vsg::MaterialFacts;
using vn::vsg::ProgramVariant;
using vn::vsg::core::CompiledCommand;
using vn::vsg::core::CompiledDraw;
using vn::vsg::core::CompiledFrame;
using vn::vsg::core::CompiledPass;
using vn::vsg::core::DrawKind;
using vn::vsg::core::FrameTimeline;
using vn::vsg::core::ProgramRef;
using vn::vsg::core::RetirementQueue;

namespace
{

/// @brief A stage of @p type, with the given source (the ASCII form the SDK stores as UTF-8).
ShaderStage stage(ShaderStageType type, const char* source)
{
    ShaderStage out;
    out.type       = type;
    out.source     = vn::String(reinterpret_cast<const char8_t*>(source));
    out.entryPoint = vn::String(reinterpret_cast<const char8_t*>("main"));
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
vn::intrusive_ptr<ShaderProgram> contentProgram()
{
    const auto program = vn::intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->addStage(stage(ShaderStageType::Vertex, kGatedVertexSource));
    program->addStage(stage(ShaderStageType::Fragment, kGatedFragmentSource));
    return program;
}

/// @brief A full-screen program: a fragment stage only (the engine brings the vertex stage).
vn::intrusive_ptr<ShaderProgram> screenProgram()
{
    const auto program = vn::intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->addStage(stage(ShaderStageType::Fragment, kScreenFragmentSource));
    return program;
}

/// @brief A quad, with the texcoord channel when @p texcoords asks for one.
vn::intrusive_ptr<Geometry> quad(bool texcoords = false)
{
    const auto geometry = vn::intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(vn::intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(
        std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F })));
    if (texcoords)
    {
        geometry->setTexcoords2(vn::intrusive_ptr<const vn::Buffer<float>>(
            new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F })));
    }
    geometry->setIndices(vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);
    return geometry;
}

/// @brief A material with a diffuse colour, and (when @p texture is not null) a texture to sample.
vn::intrusive_ptr<Material> material(vn::Colorf diffuse,
                                       vn::intrusive_ptr<Texture2D> texture = {})
{
    const auto out = vn::intrusive_ptr<Material>(new Material());
    out->setDiffuse(diffuse);
    if (texture != nullptr)
    {
        out->setTexture(texture);
    }
    return out;
}

/// @brief Reads a material entry's block back as the engine's block (the bytes ARE the shading's input).
vn::graphics::VineMaterialBlock blockOf(const MaterialFacts& facts)
{
    vn::graphics::VineMaterialBlock block{};
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
    const auto   material = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

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
    EXPECT_TRUE(findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{}, DrawKind::Content).found());
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
    const auto   material = ::material(vn::Colorf(0.1F, 0.2F, 0.3F, 1.0F));

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

TEST(ContentStoreTest, ThePublishedTablesCarryTheirRowOrder)
{
    // The store publishes a ROW ORDER beside each table (see api/ContentFacts): it is what turns the
    // recording's lookups from scans into bisections. The rows stay the truth - the order is rebuilt FROM them
    // at publication - which is why the case above can still see the same table storage every frame.
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    const auto   material = ::material(vn::Colorf(0.1F, 0.2F, 0.3F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(material);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) },
                                  program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    EXPECT_EQ(facts.program_order.size(), facts.programs.size());
    EXPECT_EQ(facts.geometry_order.size(), facts.geometries.size());
    EXPECT_EQ(facts.material_order.size(), facts.materials.size());
    ASSERT_FALSE(facts.program_order.empty()) << "one program was described, so its table has a row";

    // And the order is the one the lookups search, so the recording's own three questions answer through it.
    const auto found_program  = findProgram(facts, ProgramRef{ program.get(), program->revision() },
                                            ProgramVariant{}, DrawKind::Content);
    const auto found_geometry = findGeometry(facts, geometry.get(), geometry->revision());
    const auto found_material = findMaterial(facts, material.get());
    EXPECT_TRUE(found_program.entry != nullptr);
    EXPECT_TRUE(found_geometry.entry != nullptr);
    EXPECT_TRUE(found_material.entry != nullptr);
    EXPECT_EQ(found_geometry.entry, &facts.geometries[facts.geometry_order.front()])
        << "the answer IS the row the order names";
}

TEST(ContentStoreTest, TheRowOrderCoversEveryRowAfterAppendsAndErasures)
{
    // THE INVARIANT THE LOOKUPS TRUST: each table's row order is a permutation of that table's rows. The store
    // moves rows in four ways - a row appears (a first description), a row is REPLACED (a material edit), rows
    // are erased when a superseded revision's park comes due, and rows are erased when an abandoned object's
    // park comes due - and every one of them has to leave the order covering the table. A lookup may not prove
    // this (it falls back to scanning the table, which is correct but slow), so it is asserted HERE.
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    const auto   material = ::material(vn::Colorf(0.1F, 0.2F, 0.3F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(material);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) },
                                  program.get());
    const auto covers = [](const ContentFacts& facts) {
        const auto is_permutation = [](std::span<const std::uint32_t> order, std::size_t rows) {
            if (order.size() != rows)
            {
                return false;  // the order does not cover the table: it fell behind an append or an erase
            }
            std::vector<std::uint32_t> sorted(order.begin(), order.end());
            std::sort(sorted.begin(), sorted.end());
            for (std::size_t index = 0U; index < rows; ++index)
            {
                if (sorted[index] != index)
                {
                    return false;
                }
            }
            return true;
        };
        return is_permutation(facts.program_order, facts.programs.size()) &&
               is_permutation(facts.geometry_order, facts.geometries.size()) &&
               is_permutation(facts.material_order, facts.materials.size());
    };

    const ContentFacts& first = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(first.geometry_order.size(), first.geometries.size()) << "one row was appended: the order covers it";
    EXPECT_TRUE(covers(first));

    // A material EDIT replaces a row in place, and a geometry revision bump APPENDS one and parks the old one.
    store.updateMaterial(material.get());
    auto* mutable_material = const_cast<vn::graphics::Material*>(material.get());
    mutable_material->setDiffuse(vn::Colorf(0.9F, 0.1F, 0.1F, 1.0F));
    store.updateMaterial(material.get());
    const ContentFacts& second = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_TRUE(covers(second)) << "a replaced row keeps its place in the order";

    const std::uint64_t rows_before = static_cast<std::uint64_t>(second.geometries.size());
    const_cast<vn::graphics::Geometry*>(geometry.get())->setRevision(2U);
    ASSERT_TRUE(store.tablesFor(plan->frame, timeline, retirement).geometries.size() > rows_before)
        << "the new revision JOINS the table the old one is still answerable in";
    EXPECT_TRUE(covers(store.tablesFor(plan->frame, timeline, retirement)));

    // The parks come due: the superseded revision's rows leave, and the order has to shrink with them.
    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    const ContentFacts& pruned = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(pruned.geometries.size(), rows_before) << "the superseded revision's row has left";
    EXPECT_TRUE(covers(pruned)) << "an ERASURE has to leave the order covering exactly what is left";
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
    const auto   material = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

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
    material->setDiffuse(vn::Colorf(0.75F, 0.5F, 0.25F, 1.0F));
    store.updateMaterial(material.get());

    const ContentFacts& second = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.materialEntries(), 1U) << "a material is answered by identity alone: the row is replaced";
    const auto second_block = blockOf(*findMaterial(second, material.get()).entry);
    EXPECT_FLOAT_EQ(second_block.diffuse[0], 0.75F) << "the table answers what the material is NOW";
    EXPECT_EQ(retirement.pending(), 1U) << "the replaced value was parked";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(retirement.pending(), 0U);

    // An edit for a material nobody tracked is answered with nothing at all (it cannot be drawn either).
    const auto untracked = ::material(vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    store.updateMaterial(untracked.get());
    EXPECT_EQ(store.materialEntries(), 1U);
}

TEST(ContentStoreTest, ATouchWithNoEditChangesNothing)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    const auto   material = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(material);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) },
                                  program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);

    // A TOUCH IS A COMPARE-AND-WRITE (see ContentStore::updateMaterial): the facade touches every material a
    // frame commands, because the engine never announces an edit - so a touch that finds the values it
    // already has must build nothing and park nothing, or every frame would rebuild every material row.
    const std::uint64_t builds_before = store.builds();
    store.updateMaterial(material.get());
    const ContentFacts& again = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.builds(), builds_before) << "the touch found nothing changed";
    EXPECT_EQ(retirement.pending(), 0U) << "and nothing was parked";
    EXPECT_EQ(store.materialEntries(), 1U);
    EXPECT_FLOAT_EQ(blockOf(*findMaterial(again, material.get()).entry).diffuse[0], 0.25F);

    // The same touch AFTER an edit moves the revision, and the next walk replaces the row (the SDK's own
    // contract, now reached through the touch rather than through a separate announcement).
    material->setDiffuse(vn::Colorf(0.5F, 0.5F, 0.5F, 1.0F));
    store.updateMaterial(material.get());
    const ContentFacts& edited = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.builds(), builds_before + 1U) << "the edit replaced the row";
    EXPECT_FLOAT_EQ(blockOf(*findMaterial(edited, material.get()).entry).diffuse[0], 0.5F);
    EXPECT_EQ(retirement.pending(), 1U) << "and the value it had was parked";
}

TEST(ContentStoreTest, AProgramIsOneEntryPerVariant)
{
    ContentStore store;
    const auto   geometry   = quad(true);  // the texcoord channel is what a cube variant would widen
    const auto   program    = contentProgram();
    const auto   texture    = vn::intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const auto   textured   = ::material(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F), texture);
    const auto   plain      = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

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

    const auto textured_entry = findProgram(facts, ProgramRef{ program.get(), program->revision() }, textured_variant, DrawKind::Content);
    const auto plain_entry    = findProgram(facts, ProgramRef{ program.get(), program->revision() }, plain_variant, DrawKind::Content);
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
    // Asked for as the call it was described FOR: this program's entry answers the full-screen ABI (see
    // ProgramFacts::kind), and asking for the content one is a miss rather than the other build.
    const auto entry =
        findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{}, DrawKind::Screen);
    ASSERT_TRUE(entry.found());
    EXPECT_EQ(entry.entry->kind, DrawKind::Screen);
    EXPECT_NE(entry.entry->shaders.vertex.find("gl_VertexIndex"), std::string::npos)
        << "the full-screen ABI's vertex stage is the engine's triangle, not the host's";
    EXPECT_FALSE(findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{},
                             DrawKind::Content)
                     .found())
        << "a program only ever described as a screen call has no content entry";
}

TEST(ContentStoreTest, AnUntrackedObjectIsSimplyAbsent)
{
    ContentStore store;

    // The objects exist and the plan names them, but the host never handed them over: a store cannot build
    // facts from an address alone, and it does not pretend to.
    const auto geometry = quad();
    const auto program  = contentProgram();
    const auto material = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

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
    EXPECT_EQ(findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{}, DrawKind::Content).miss, FactMiss::Unknown);
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

TEST(ContentStoreTest, OneProgramDescribedAsBothKindsIsNeverServedForTheOther)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();  // both stages: describable as content AND as a screen call

    store.track(geometry);
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    // One frame shades CONTENT with it...
    const auto content_plan =
        contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) }, program.get());
    (void)store.tablesFor(content_plan->frame, timeline, retirement);
    ASSERT_EQ(store.programEntries(), 1U);

    // ... and a later frame draws the SAME program as a full-screen call. The two are different ABIs, so the
    // table has to answer with two entries - and a lookup that did not name the kind would hand whichever row
    // came first to a caller expecting the other, which is a wrong picture with no refusal anywhere.
    const auto          screen_plan = screenPlan(program.get());
    const ContentFacts& facts       = store.tablesFor(screen_plan->frame, timeline, retirement);

    ASSERT_EQ(store.programEntries(), 2U) << "one text, two entries: the two calls are different ABIs";
    const auto content =
        findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{}, DrawKind::Content);
    const auto screen =
        findProgram(facts, ProgramRef{ program.get(), program->revision() }, ProgramVariant{}, DrawKind::Screen);
    ASSERT_TRUE(content.found());
    ASSERT_TRUE(screen.found());
    EXPECT_NE(content.entry, screen.entry) << "each call gets its own entry";
    EXPECT_EQ(content.entry->kind, DrawKind::Content);
    EXPECT_EQ(screen.entry->kind, DrawKind::Screen);
    // And the entries really are those two builds: the content one carries the HOST's vertex stage, the screen
    // one the engine's generated triangle (see api/ContentSources).
    EXPECT_EQ(content.entry->shaders.vertex, std::string(kGatedVertexSource));
    EXPECT_NE(screen.entry->shaders.vertex.find("gl_VertexIndex"), std::string::npos)
        << "the full-screen entry's vertex stage is the engine's, not the host's";
}
