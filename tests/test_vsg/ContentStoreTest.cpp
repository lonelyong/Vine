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

#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/ContentStore.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>
#include <vine/vsg/internal/StreamUploads.hpp>

#include "TestGeometryBuilders.hpp"
#include "TestProgramFacts.hpp"

using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::backend::CompiledCommand;
using vn::graphics::backend::CompiledDraw;
using vn::graphics::backend::CompiledFrame;
using vn::graphics::backend::CompiledPass;
using vn::graphics::backend::DrawKind;
using vn::graphics::backend::FrameTimeline;
using vn::graphics::backend::RetirementQueue;
using vn::test::setPrimitiveIndices;
using vn::vsg::ContentFacts;
using vn::vsg::ContentStore;
using vn::vsg::FactMiss;
using vn::vsg::findGeometry;
using vn::vsg::findMaterial;
using vn::vsg::findProgram;
using vn::vsg::GeometryFacts;
using vn::vsg::MaterialFacts;
using vn::vsg::programName;
using vn::vsg::ProgramVariant;
using vn::vsg::StreamUploads;

namespace
{

/// @brief The material lookup a case writes: the plan names (id, revision), so a case holding the object asks with its pair.
///
/// A NAMED CONVERSION, and the same one production makes: `FrameRecorder` builds the reference from the object's id and
/// the revision of the values it published. Overloading the free function keeps the cases reading like the lookup they
/// mean without giving the table a second identity.
vn::vsg::FactResult<vn::vsg::MaterialFacts> findMaterial(const ContentFacts& facts, const vn::graphics::MaterialBase* material)
{
    if (material == nullptr) {
        return vn::vsg::findMaterial(facts, vn::graphics::ContentRef{});
    }
    return vn::vsg::findMaterial(facts, vn::graphics::ContentRef{ material->id().handle(), material->revision() });
}

/// @brief A stage of @p type, with the given source (the ASCII form the SDK stores as UTF-8).
ShaderStage stage(ShaderStageType type, const char* source)
{
    ShaderStage out;
    out.type       = type;
    out.source     = vn::String(reinterpret_cast<const char8_t*>(source));
    out.entryPoint = vn::String(reinterpret_cast<const char8_t*>("main"));
    return out;
}

/// @brief A content program whose fragment stage gates its sampler on `VN_TEXTURED`: one text, two ABIs.
constexpr const char* kGatedVertexSource = "#version 450\n"
                                           "#pragma import_defines (VN_TEXTURED)\n"
                                           "layout(location = 0) in vec3 position;\n"
                                           "#ifdef VN_TEXTURED\n"
                                           "layout(location = 8) in vec2 texcoord;\n"
                                           "#endif\n"
                                           "void main() { gl_Position = vec4(position, 1.0); }\n";

constexpr const char* kGatedFragmentSource = "#version 450\n"
                                             "#pragma import_defines (VN_TEXTURED)\n"
                                             "layout(location = 0) out vec4 color;\n"
                                             "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
                                             "{\n"
                                             "    vec4 base_color; vec4 emissive; vec4 params; uvec4 flags;\n"
                                             "} material;\n"
                                             "#ifdef VN_TEXTURED\n"
                                             "layout(set = 0, binding = 1) uniform sampler2D vn_baseColorMap;\n"
                                             "#endif\n"
                                             "void main() { color = material.base_color; }\n";

constexpr const char* kScreenFragmentSource = "#version 450\n"
                                              "layout(location = 0) in vec2 vn_uv;\n"
                                              "layout(location = 0) out vec4 color;\n"
                                              "void main() { color = vec4(vn_uv, 0.0, 1.0); }\n";

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

/// @brief A content program that says when it dies: what the store's ADDRESS RESERVATION is observed with.
///
/// The store keys every facts row by the object's address, so an object whose rows can still be read must not
/// be destroyed - the allocator would otherwise hand the same address to the next object, whose fresh rows the
/// old identity's park then erases (see ContentStore::releaseAbandoned).
class CountedProgram final : public ShaderProgram {
  public:
    CountedProgram()
    {
        addStage(::stage(ShaderStageType::Vertex, kGatedVertexSource));
        addStage(::stage(ShaderStageType::Fragment, kGatedFragmentSource));
    }

    ~CountedProgram() override
    {
        ++destructions;
    }

  public:
    inline static int destructions = 0;
};

/// @brief A quad, with the texcoord channel when @p texcoords asks for one.
vn::intrusive_ptr<Geometry> quad(bool texcoords = false)
{
    const auto geometry = vn::intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(vn::intrusive_ptr<const vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F })));
    if (texcoords) {
        geometry->setTexcoords2(
            vn::intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F })));
    }
    setPrimitiveIndices(
        *geometry,
        0U,
        vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);
    return geometry;
}

/// @brief A material with a diffuse colour, and (when @p texture is not null) a texture to sample.
vn::intrusive_ptr<Material> material(vn::Colorf diffuse, vn::intrusive_ptr<Texture2D> texture = {})
{
    const auto out = vn::intrusive_ptr<Material>(new Material());
    out->setBaseColor(diffuse);
    if (texture != nullptr) {
        out->setTexture(vn::graphics::TextureSlot::BaseColor, texture);
    }
    return out;
}

/// @brief Reads a material entry's block back as the engine's block (the bytes ARE the shading's input).
vn::graphics::VineMaterialBlock blockOf(const MaterialFacts& facts)
{
    vn::graphics::VineMaterialBlock block{};
    if (facts.block.size() == sizeof(block)) {
        std::memcpy(&block, facts.block.data(), sizeof(block));
    }
    return block;
}

/// @brief One frame's plan, built by hand: the store walks identities, and a hand-built plan is the
/// smallest thing that has them.
struct Plan {
    std::vector<CompiledCommand> commands;
    std::vector<CompiledDraw>    draws;
    std::vector<CompiledPass>    passes;
    CompiledFrame                frame{};
};

/// @brief A content command naming @p geometry (id and revision, as one plan row keys them), @p program and @p material.
CompiledCommand command(vn::graphics::ContentRef geometry, const ShaderProgram* program, vn::graphics::ContentRef material)
{
    CompiledCommand out;
    out.geometry = geometry;
    out.program  = program != nullptr ? vn::test::programRef(*program) : vn::graphics::ContentRef{};
    out.material = material;
    return out;
}

/** @brief The same command naming a material by the OBJECT a case holds: the plan carries the id and the revision. */
CompiledCommand command(vn::graphics::ContentRef geometry, const ShaderProgram* program, const vn::graphics::MaterialBase* material)
{
    const vn::graphics::ContentRef ref =
        material != nullptr ? vn::graphics::ContentRef{ material->id().handle(), material->revision() } : vn::graphics::ContentRef{};
    return command(geometry, program, ref);
}

/** @brief The same command named by the OBJECT a case holds: a plan carries the geometry's id and the revision its
 *         shape is at (a `Geometry` inherits its id from `Node`), so the two halves make one ref here. */
CompiledCommand command(const vn::graphics::Geometry* geometry, std::uint64_t geometry_revision, const ShaderProgram* program, const vn::graphics::MaterialBase* material)
{
    const vn::graphics::NodeId identity = geometry != nullptr ? geometry->id() : vn::graphics::NodeId{};
    return command(vn::graphics::contentRefOf(identity, geometry_revision), program, material);
}

/// @brief The ref a lookup names: the geometry's identity and the revision the row must answer for.
vn::graphics::ContentRef geometryRef(vn::graphics::NodeId identity, std::uint64_t revision)
{
    return vn::graphics::contentRefOf(identity, revision);
}

/// @brief A plan with one pass and one content draw over @p commands.
std::unique_ptr<Plan> contentPlan(std::vector<CompiledCommand> commands, const ShaderProgram* program = nullptr)
{
    auto plan      = std::make_unique<Plan>();
    plan->commands = std::move(commands);

    CompiledDraw draw;
    draw.kind     = DrawKind::Content;
    draw.commands = plan->commands;
    plan->draws.push_back(draw);

    CompiledPass pass;
    pass.draws = plan->draws;
    plan->passes.push_back(pass);
    plan->frame.passes = plan->passes;
    if (program != nullptr) {
        plan->frame.default_program = vn::test::programRef(*program);
    }
    return plan;
}

/// @brief A plan with one pass and one full-screen draw of @p program.
std::unique_ptr<Plan> screenPlan(const ShaderProgram* program)
{
    auto plan = std::make_unique<Plan>();

    CompiledDraw draw;
    draw.kind    = DrawKind::Screen;
    draw.program = vn::test::programRef(*program);
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

} // namespace

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

    const auto          plan  = contentPlan({ command(named.get(), named->revision(), program.get(), material.get()) }, program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    EXPECT_EQ(store.geometryEntries(), 1U) << "the geometry no plan names is not built";
    EXPECT_EQ(store.programEntries(), 1U);
    EXPECT_EQ(store.materialEntries(), 1U);
    EXPECT_EQ(store.builds(), 3U);

    EXPECT_TRUE(findGeometry(facts, geometryRef(named->id(), named->revision())).found());
    EXPECT_TRUE(findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Content).found());
    EXPECT_TRUE(findMaterial(facts, material.get()).found());

    // A second frame naming the other three builds the other three, and nothing else.
    const auto          other_plan = contentPlan({ command(unnamed.get(), unnamed->revision(), program.get(), nullptr) }, program.get());
    const ContentFacts& second     = store.tablesFor(other_plan->frame, timeline, retirement);
    EXPECT_EQ(store.geometryEntries(), 2U);
    EXPECT_EQ(store.programEntries(), 1U);
    EXPECT_EQ(store.materialEntries(), 2U) << "the default material joins the table the first time content without one is named";
    EXPECT_EQ(store.builds(), 5U) << "the other geometry and the default material, and nothing else (the program is already answered)";
    EXPECT_TRUE(findMaterial(second, vn::graphics::ContentRef{}).found());
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

    const auto          plan  = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);
    const std::uint64_t built = store.builds();

    for (int frame = 0; frame < 3; ++frame) {
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

    const auto          plan  = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    EXPECT_EQ(facts.program_order.size(), facts.programs.size());
    EXPECT_EQ(facts.geometry_order.size(), facts.geometries.size());
    EXPECT_EQ(facts.material_order.size(), facts.materials.size());
    ASSERT_FALSE(facts.program_order.empty()) << "one program was described, so its table has a row";

    // And the order is the one the lookups search, so the recording's own three questions answer through it.
    const auto found_program  = findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Content);
    const auto found_geometry = findGeometry(facts, geometryRef(geometry->id(), geometry->revision()));
    const auto found_material = findMaterial(facts, material.get());
    EXPECT_TRUE(found_program.entry != nullptr);
    EXPECT_TRUE(found_geometry.entry != nullptr);
    EXPECT_TRUE(found_material.entry != nullptr);
    EXPECT_EQ(found_geometry.entry, &facts.geometries[facts.geometry_order.front()]) << "the answer IS the row the order names";
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

    const auto plan   = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    const auto covers = [](const ContentFacts& facts) {
        const auto is_permutation = [](std::span<const std::uint32_t> order, std::size_t rows) {
            if (order.size() != rows) {
                return false; // the order does not cover the table: it fell behind an append or an erase
            }
            std::vector<std::uint32_t> sorted(order.begin(), order.end());
            std::sort(sorted.begin(), sorted.end());
            for (std::size_t index = 0U; index < rows; ++index) {
                if (sorted[index] != index) {
                    return false;
                }
            }
            return true;
        };
        return is_permutation(facts.program_order, facts.programs.size()) && is_permutation(facts.geometry_order, facts.geometries.size()) &&
               is_permutation(facts.material_order, facts.materials.size());
    };

    const ContentFacts& first = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(first.geometry_order.size(), first.geometries.size()) << "one row was appended: the order covers it";
    EXPECT_TRUE(covers(first));

    // A material EDIT replaces a row in place, and a geometry revision bump APPENDS one and parks the old one.
    store.track(material.get(), material->description());
    auto* mutable_material = const_cast<vn::graphics::Material*>(material.get());
    mutable_material->setBaseColor(vn::Colorf(0.9F, 0.1F, 0.1F, 1.0F));
    store.track(material.get(), material->description());
    const ContentFacts& second = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_TRUE(covers(second)) << "a replaced row keeps its place in the order";

    const std::uint64_t rows_before = static_cast<std::uint64_t>(second.geometries.size());
    const_cast<vn::graphics::Geometry*>(geometry.get())->setRevision(2U);
    store.track(geometry); // the frame hands over the NEW shape (a description is a value; an edit is a new one)
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

    const auto          plan  = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) }, program.get());
    const ContentFacts& first = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_TRUE(findGeometry(first, geometryRef(geometry->id(), geometry->revision())).found());
    const std::uint64_t old_revision = geometry->revision();

    // The host edits the data and announces it (the SDK's contract): the plan still names the old revision.
    geometry->bumpRevision();
    store.track(geometry); // the next frame hands the NEW shape over (see ContentStore::track)
    const std::uint64_t new_revision = geometry->revision();
    EXPECT_NE(new_revision, old_revision);

    const ContentFacts& second = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.geometryEntries(), 2U) << "the new revision JOINS the table, it does not replace it";
    EXPECT_TRUE(findGeometry(second, geometryRef(geometry->id(), new_revision)).found()) << "the live revision answers";
    EXPECT_TRUE(findGeometry(second, geometryRef(geometry->id(), old_revision)).found())
        << "and so does the revision a recorded frame may still name";
    EXPECT_EQ(retirement.pending(), 1U) << "the supersession was parked";

    // Not yet due: the park's window is the slots that could still record the old revision.
    releaseUpTo(timeline, retirement, 1U);
    EXPECT_EQ(retirement.pending(), 1U) << "released a frame early";
    EXPECT_TRUE(findGeometry(second, geometryRef(geometry->id(), old_revision)).found());

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(retirement.pending(), 0U);
    EXPECT_EQ(store.geometryEntries(), 1U) << "the superseded revision left the table";
    EXPECT_EQ(findGeometry(store.tablesFor(plan->frame, timeline, retirement), geometryRef(geometry->id(), old_revision)).miss, FactMiss::Revision)
        << "a plan naming it now misses with Revision (the identity is known, that revision is not)";
}

TEST(ContentStoreTest, AMaterialEditJoinsTheTableAndTheOldRevisionLeavesAtThePark)
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

    const auto          plan        = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    const ContentFacts& first       = store.tablesFor(plan->frame, timeline, retirement);
    const auto          first_block = blockOf(*findMaterial(first, material.get()).entry);
    EXPECT_FLOAT_EQ(first_block.base_color[0], 0.25F);
    const std::uint64_t old_revision = material->revision();
    const vn::graphics::ContentRef old_ref{ material->id().handle(), old_revision };

    // The host edits the material. The material moves its own revision (Material::revision - every shading value
    // goes through a setter, which compares first), and the facade touches every material a frame commands because
    // the frame publishes the values its row carried; this is that publish.
    const std::uint64_t before = material->revision();
    material->setBaseColor(vn::Colorf(0.75F, 0.5F, 0.25F, 1.0F));
    EXPECT_EQ(material->revision(), before + 1U);
    store.track(material.get(), material->description());
    const std::uint64_t new_revision = material->revision();
    ASSERT_NE(new_revision, old_revision);

    // A frame compiled BEFORE the edit still names the old revision, and the table has to keep answering it - the
    // same rule the geometry table follows (see the case above), and the one A1 was about.
    const ContentFacts& second = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.materialEntries(), 2U) << "the new revision JOINS the table, it does not replace the old one";
    EXPECT_TRUE(findMaterial(second, old_ref).found()) << "the revision a recorded frame may still name keeps answering";
    const auto second_block = blockOf(*findMaterial(second, material.get()).entry);
    EXPECT_FLOAT_EQ(second_block.base_color[0], 0.75F) << "and the new one answers what the material is NOW";
    EXPECT_EQ(retirement.pending(), 1U) << "the supersession was parked";

    // Not yet due: the park's window is the slots that could still record the old revision.
    releaseUpTo(timeline, retirement, 1U);
    EXPECT_EQ(retirement.pending(), 1U) << "released a frame early";
    EXPECT_TRUE(findMaterial(second, old_ref).found());

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(retirement.pending(), 0U);
    EXPECT_EQ(store.materialEntries(), 1U) << "the superseded revision left the table";

    // An edit for a material nobody tracked is answered with nothing at all (it cannot be drawn either).
    const auto untracked = ::material(vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    store.track(untracked.get(), untracked->description());
    EXPECT_EQ(store.materialEntries(), 1U);
}

TEST(ContentStoreTest, AWrongMaterialSlotIsReportedOncePerEpisode)
{
    // graphics-material.md F11 / pitfall P82: the sentence about a wrong slot belongs to the recorder, which is
    // rebuilt every frame - so the MEMORY has to live somewhere that is not. It lives here, one bit per slot in
    // the material's own entry, and an edit re-arms it (the values changed, so what is wrong with them may have).
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    const auto   material = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(material);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement); // the first description opens the episode

    EXPECT_TRUE(store.claimSlotReport(material->id(), 2U)) << "the first frame says it";
    EXPECT_FALSE(store.claimSlotReport(material->id(), 2U)) << "the second does not";
    EXPECT_TRUE(store.claimSlotReport(material->id(), 3U)) << "another slot is its own report";

    // An edit is a new episode: a host that fixed one map and broke another hears about the second.
    material->setBaseColor(vn::Colorf(0.5F, 0.5F, 0.5F, 1.0F));
    store.track(material.get(), material->description());
    EXPECT_TRUE(store.claimSlotReport(material->id(), 2U)) << "the material changed: the episode restarted";
    EXPECT_FALSE(store.claimSlotReport(material->id(), 2U));

    // An untracked material has no record to remember it in: the answer is "report it" rather than silence - the
    // recorder's own per-pass state is what keeps that from repeating every frame.
    const auto untracked = ::material(vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    EXPECT_TRUE(store.claimSlotReport(untracked->id(), 2U));
    EXPECT_TRUE(store.claimSlotReport(untracked->id(), 2U)) << "and it stays that way: there is nothing to record";
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

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);

    // A PUBLISHED VALUE IS ONE NUMBER: every frame hands the store the value its row carried, so a
    // value whose revision is the one the row was built at must build nothing and park nothing, or every
    // frame would rebuild every material row. Re-applying the values the material already holds is the
    // other half of the same guarantee: it does not move the revision either.
    const std::uint64_t builds_before = store.builds();
    for (int frame = 0; frame < 5; ++frame) {
        material->setBaseColor(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F)); // same value: not an edit
        store.track(material.get(), material->description());
        (void)store.tablesFor(plan->frame, timeline, retirement);
    }
    const ContentFacts& again = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.builds(), builds_before) << "the touch found nothing changed";
    EXPECT_EQ(retirement.pending(), 0U) << "and nothing was parked";
    EXPECT_EQ(store.materialEntries(), 1U);
    EXPECT_FLOAT_EQ(blockOf(*findMaterial(again, material.get()).entry).base_color[0], 0.25F);

    // The same touch AFTER an edit moves the revision, and the next walk replaces the row (the SDK's own
    // contract, now reached through the touch rather than through a separate announcement).
    material->setBaseColor(vn::Colorf(0.5F, 0.5F, 0.5F, 1.0F));
    store.track(material.get(), material->description());
    const ContentFacts& edited = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.builds(), builds_before + 1U) << "the edit replaced the row";
    EXPECT_FLOAT_EQ(blockOf(*findMaterial(edited, material.get()).entry).base_color[0], 0.5F);
    EXPECT_EQ(retirement.pending(), 1U) << "and the value it had was parked";
}

TEST(ContentStoreTest, AProgramIsOneEntryPerVariant)
{
    ContentStore store;
    const auto   geometry = quad(true); // the texcoord channel is what a cube variant would widen
    const auto   program  = contentProgram();
    const auto   texture  = vn::intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const auto   textured = ::material(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F), texture);
    const auto   plain    = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    store.track(geometry);
    store.track(program);
    store.track(textured);
    store.track(plain);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto          plan  = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), textured.get()),
                                              command(geometry.get(), geometry->revision(), program.get(), plain.get()) },
                                            program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    ASSERT_EQ(store.programEntries(), 2U) << "one program, two texts: two entries";

    ProgramVariant textured_variant;
    textured_variant.textured = true;
    const ProgramVariant plain_variant;

    const auto textured_entry = findProgram(facts, vn::test::programRef(*program), textured_variant, DrawKind::Content);
    const auto plain_entry    = findProgram(facts, vn::test::programRef(*program), plain_variant, DrawKind::Content);
    ASSERT_TRUE(textured_entry.found());
    ASSERT_TRUE(plain_entry.found());
    EXPECT_EQ(textured_entry.entry->abi.bindings.size(), plain_entry.entry->abi.bindings.size() + 1U) << "the sampler is what the material's texture adds";
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
    const auto entry = findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Screen);
    ASSERT_TRUE(entry.found());
    EXPECT_EQ(entry.entry->kind, DrawKind::Screen);
    EXPECT_NE(entry.entry->shaders.vertex.find("gl_VertexIndex"), std::string::npos)
        << "the full-screen ABI's vertex stage is the engine's triangle, not the host's";
    EXPECT_FALSE(findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Content).found())
        << "a program only ever described as a screen call has no content entry";
}

TEST(ContentStoreTest, AProgramThatCannotBeDescribedStillHasItsName)
{
    ContentStore                     store;
    vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    program->setName(u8"broken");
    ShaderStage fragment;
    fragment.type = ShaderStageType::Fragment; // empty source: not a program
    program->addStage(fragment);
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const auto          plan  = screenPlan(program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    // No row: a program that is not one content pipeline is refused, and the recorder reports the miss...
    EXPECT_FALSE(findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Screen).found());
    // ... and the sentence can still say WHICH program, because the name is recorded when the identity is described.
    EXPECT_EQ(programName(facts, program->id()), std::string_view("broken"));
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

    const auto          plan  = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);

    EXPECT_EQ(store.geometryEntries(), 0U);
    EXPECT_EQ(store.programEntries(), 0U);
    EXPECT_EQ(store.materialEntries(), 0U);
    EXPECT_EQ(store.builds(), 0U);
    EXPECT_EQ(findGeometry(facts, geometryRef(geometry->id(), geometry->revision())).miss, FactMiss::Unknown);
    EXPECT_EQ(findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Content).miss, FactMiss::Unknown);
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

    const auto plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);
    ASSERT_EQ(store.programEntries(), 1U);

    std::vector<vn::graphics::MaterialId> released_materials;

    // The host still holds both, so neither is abandoned.
    EXPECT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 0U);
    EXPECT_EQ(store.geometryEntries(), 1U);
    EXPECT_TRUE(released_materials.empty());

    // The host lets the geometry go: the store's share is the last one.
    const auto          identity = geometry->id();
    const std::uint64_t revision = geometry->revision();
    geometry.reset();
    EXPECT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 1U);
    EXPECT_EQ(retirement.pending(), 1U) << "the abandoned rows are parked, not dropped: a recorded frame may still name them";
    EXPECT_TRUE(findGeometry(store.tablesFor(plan->frame, timeline, retirement), geometryRef(identity, revision)).found())
        << "the rows still answer while the parking window is open";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.geometryEntries(), 0U);
    EXPECT_EQ(store.programEntries(), 1U) << "the program is still held by the host";
}

// 地址就是行的键：一个对象的行还可能被读到（停靠在排队窗口里的那些），它的地址就不能被释放 —— 否则下一个对象
// 会拿到同一地址，(a) 新对象会被旧行当成旧对象描述，(b) 旧身份的那次移除会把新对象的行一起擦掉。后者实测过一次
// "整条 pass 再没有内容半边"（换一次着色模式后画面永远不再更新），所以这条用例把保留本身钉住。
TEST(ContentStoreTest, AParkedRemovalKeepsTheObjectsAddressReservedUntilItsRowsLeave)
{
    ContentStore store;
    CountedProgram::destructions = 0;

    auto geometry = quad();
    auto shading  = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));
    auto program  = vn::intrusive_ptr<ShaderProgram>(new CountedProgram());

    store.track(geometry);
    store.track(program);
    store.track(shading);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), shading.get()) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.programEntries(), 1U);

    const void* const        address = program.get();
    std::vector<vn::graphics::MaterialId> released_materials;
    program.reset();
    geometry.reset();
    shading.reset(); // the store's shares are the last ones (the plan copies identities, not handles)

    EXPECT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 3U) << "the geometry, the program and the material";
    EXPECT_EQ(CountedProgram::destructions, 0) << "the park owns the object, so the address its rows are keyed by stays reserved";

    // An object allocated NOW cannot take that address: the rows still answer for it.
    const auto later = vn::intrusive_ptr<ShaderProgram>(new CountedProgram());
    EXPECT_NE(later.get(), address) << "the reserved address is not handed to a new object while its rows can still be read";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.programEntries(), 0U);
    EXPECT_EQ(CountedProgram::destructions, 1) << "the park dropped the object together with the rows it guarded";
}

// 停靠排不上（会话还不知道 in-flight 数）时，行的键同样是地址：条目必须连对象一起留着，下一拍再试。"只计数、
// 不保留"就是这条用例要抓的形态——那时地址已经被交给分配器，而行还在表里。
TEST(ContentStoreTest, AReleaseThatCannotBeParkedYetKeepsItsEntryAndIsRetried)
{
    ContentStore store;
    CountedProgram::destructions = 0;

    const auto geometry = quad();
    const auto shading  = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));
    auto       program  = vn::intrusive_ptr<ShaderProgram>(new CountedProgram());

    store.track(geometry);
    store.track(program);
    store.track(shading);

    FrameTimeline   timeline;
    RetirementQueue retirement(0U); // the session has not learned its in-flight count: parking is OFF
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), shading.get()) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.programEntries(), 1U);

    std::vector<vn::graphics::MaterialId> released_materials;
    program.reset();

    EXPECT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 0U) << "nothing can be parked, so nothing is let go";
    EXPECT_EQ(retirement.pending(), 0U);
    EXPECT_EQ(CountedProgram::destructions, 0) << "the entry keeps the object, so the address its rows are keyed by stays reserved";
    EXPECT_EQ(store.programEntries(), 1U) << "and the rows stay answerable for the plan that still names them";

    // The session learns how many frames may be in flight: the SAME entry is retried, and parks now.
    retirement.setSlots(1U);
    EXPECT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 1U) << "the retry is what parks it";
    EXPECT_EQ(retirement.pending(), 1U);
    EXPECT_EQ(CountedProgram::destructions, 0) << "parked, not freed";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.programEntries(), 0U);
    EXPECT_EQ(CountedProgram::destructions, 1) << "the park took the object with the rows";
}

TEST(ContentStoreTest, AReleaseIsCancelledByTheNextUse)
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
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);

    ASSERT_TRUE(store.releaseGeometry(geometry.get(), timeline, retirement));
    EXPECT_EQ(retirement.pending(), 1U) << "the release waits for the in-flight window";

    // The next frame uses it again: the pending callback must become a no-op, not erase the object's rows.
    store.track(geometry);
    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.geometryEntries(), 1U);
    EXPECT_TRUE(findGeometry(store.tablesFor(plan->frame, timeline, retirement), geometryRef(geometry->id(), geometry->revision())).found());
}

TEST(ContentStoreTest, AReleaseWithoutReuseDropsTheRowsAtThePark)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    store.track(geometry);
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);

    ASSERT_TRUE(store.releaseGeometry(geometry.get(), timeline, retirement));
    EXPECT_EQ(retirement.pending(), 1U);
    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.geometryEntries(), 0U);
    EXPECT_EQ(retirement.released(), 1U);

    // The object was not destroyed: it is untracked, so the next frame that names it can hand it over again.
    const std::uint64_t builds = store.builds();
    store.track(geometry);
    (void)store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.builds(), builds + 1U);
}

TEST(ContentStoreTest, ASecondReleaseDoesNotParkASecondCallback)
{
    ContentStore store;
    const auto   geometry = quad();
    store.track(geometry);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    ASSERT_TRUE(store.releaseGeometry(geometry.get(), timeline, retirement));
    ASSERT_TRUE(store.releaseGeometry(geometry.get(), timeline, retirement));
    EXPECT_EQ(retirement.pending(), 1U) << "one request is enough";
}

TEST(ContentStoreTest, AReleaseWithoutAParkingWindowChangesNothing)
{
    ContentStore store;
    const auto   geometry = quad();
    store.track(geometry);

    FrameTimeline   timeline;
    RetirementQueue retirement(0U); // the session has not learned its in-flight count
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), nullptr, nullptr) });
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.geometryEntries(), 1U);

    EXPECT_FALSE(store.releaseGeometry(geometry.get(), timeline, retirement)) << "a release that cannot be parked must be refused, not guessed";
    EXPECT_EQ(retirement.pending(), 0U);
    EXPECT_EQ(store.geometryEntries(), 1U);

    // It is still a live object: the next walk must not find a half-released entry.
    (void)store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.geometryEntries(), 1U);
}

TEST(ContentStoreTest, AReleaseThenAnEditKeepsTheNewRevisionAfterThePark)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram();
    store.track(geometry);
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    const std::uint64_t old_revision = geometry->revision();

    ASSERT_TRUE(store.releaseGeometry(geometry.get(), timeline, retirement));

    // The object is edited and used again before the release callback runs: the new revision must survive.
    const_cast<vn::graphics::Geometry*>(geometry.get())->bumpRevision();
    const std::uint64_t new_revision = geometry->revision();
    store.track(geometry);
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_NE(new_revision, old_revision);
    EXPECT_TRUE(findGeometry(facts, geometryRef(geometry->id(), new_revision)).found());

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    const ContentFacts& after = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_TRUE(findGeometry(after, geometryRef(geometry->id(), new_revision)).found())
        << "the cancelled release must not erase the revision the host is using now";
    EXPECT_EQ(store.geometryEntries(), 1U);
}

TEST(ContentStoreTest, AReleaseOfAProgramOrMaterialDropsItsRows)
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
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), material.get()) }, program.get());
    (void)store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(store.programEntries(), 1U);
    ASSERT_EQ(store.materialEntries(), 1U);

    ASSERT_TRUE(store.releaseProgram(program.get(), timeline, retirement));
    ASSERT_TRUE(store.releaseMaterial(material.get(), timeline, retirement));
    EXPECT_EQ(retirement.pending(), 2U);

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    EXPECT_EQ(store.programEntries(), 0U);
    EXPECT_EQ(store.materialEntries(), 0U);
    EXPECT_EQ(store.geometryEntries(), 1U) << "the untouched geometry stays";
}

TEST(ContentStoreTest, OneProgramDescribedAsBothKindsIsNeverServedForTheOther)
{
    ContentStore store;
    const auto   geometry = quad();
    const auto   program  = contentProgram(); // both stages: describable as content AND as a screen call

    store.track(geometry);
    store.track(program);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    // One frame shades CONTENT with it...
    const auto content_plan = contentPlan({ command(geometry.get(), geometry->revision(), program.get(), nullptr) }, program.get());
    (void)store.tablesFor(content_plan->frame, timeline, retirement);
    ASSERT_EQ(store.programEntries(), 1U);

    // ... and a later frame draws the SAME program as a full-screen call. The two are different ABIs, so the
    // table has to answer with two entries - and a lookup that did not name the kind would hand whichever row
    // came first to a caller expecting the other, which is a wrong picture with no refusal anywhere.
    const auto          screen_plan = screenPlan(program.get());
    const ContentFacts& facts       = store.tablesFor(screen_plan->frame, timeline, retirement);

    ASSERT_EQ(store.programEntries(), 2U) << "one text, two entries: the two calls are different ABIs";
    const auto content = findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Content);
    const auto screen  = findProgram(facts, vn::test::programRef(*program), ProgramVariant{}, DrawKind::Screen);
    ASSERT_TRUE(content.found());
    ASSERT_TRUE(screen.found());
    EXPECT_NE(content.entry, screen.entry) << "each call gets its own entry";
    EXPECT_EQ(content.entry->kind, DrawKind::Content);
    EXPECT_EQ(screen.entry->kind, DrawKind::Screen);
    // And the entries really are those two builds: the content one carries the HOST's vertex stage, the screen
    // one the engine's generated triangle (see api/ContentSources).
    EXPECT_EQ(content.entry->shaders.vertex, std::string(kGatedVertexSource));
    EXPECT_NE(screen.entry->shaders.vertex.find("gl_VertexIndex"), std::string::npos) << "the full-screen entry's vertex stage is the engine's, not the host's";
}

namespace
{

/// @brief Binds every stream a facts row declares, the way the recorder does; answers how many were uploads.
std::uint64_t bindStreams(StreamUploads& uploads, const vn::vsg::GeometryFacts& row)
{
    std::uint64_t built = 0U;
    for (const vn::vsg::ChannelFacts& channel : row.channels) {
        built += uploads.acquireVertex(channel.streamKey(), channel.scalars(), channel.values).action == StreamUploads::Action::Uploaded ? 1U : 0U;
    }
    for (const vn::vsg::PrimitiveFacts& primitive : row.primitives) {
        if (!primitive.indices.has_value()) {
            continue;
        }
        built += uploads.acquireIndex(primitive.indices->streamKey(), primitive.indices->scalars(), primitive.indices->values).action == StreamUploads::Action::Uploaded ? 1U : 0U;
    }
    return built;
}

} // namespace

TEST(ContentStoreTest, ACulledGeometryKeepsItsUploadsForAHundredFrames)
{
    ContentStore store;
    const auto   geometry = quad();
    store.track(geometry);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      drawing = contentPlan({ command(geometry.get(), geometry->revision(), nullptr, nullptr) });
    const auto      culled  = contentPlan({});

    StreamUploads       uploads;
    const ContentFacts& drawn = store.tablesFor(drawing->frame, timeline, retirement);
    ASSERT_EQ(drawn.geometries.size(), 1U);
    (void)uploads.beginFrame(drawn, timeline, retirement);
    ASSERT_EQ(bindStreams(uploads, drawn.geometries.front()), 2U) << "positions + the index arena";

    // CULLING: the plan names nothing, but the geometry is still tracked, so its row - the declaration - stays.
    for (int frame = 0; frame < 100; ++frame) {
        const ContentFacts& facts = store.tablesFor(culled->frame, timeline, retirement);
        ASSERT_EQ(facts.geometries.size(), 1U) << "frame " << frame << ": the tracked geometry's row is still the declaration";
        (void)uploads.beginFrame(facts, timeline, retirement);
    }

    EXPECT_EQ(uploads.uploads(), 2U) << "a hundred culled frames uploaded nothing";
    EXPECT_EQ(uploads.live(), 2U);
    EXPECT_EQ(uploads.retired(), 0U) << "nothing left: the geometry was only out of view";
}

TEST(ContentStoreTest, AStreamLeavesWithinTheParkingWindowAfterItsGeometryIsDropped)
{
    ContentStore store;
    auto         geometry = quad();
    store.track(geometry);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), nullptr, nullptr) });

    StreamUploads       uploads;
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(bindStreams(uploads, facts.geometries.front()), 2U);
    ASSERT_EQ(uploads.live(), 2U);

    // The host lets go of its last reference; the sweep parks the row removal (the replaced-object window).
    geometry.reset();
    std::vector<vn::graphics::MaterialId> released_materials;
    ASSERT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 1U);

    // Inside the window the PIN is still there, because the row is parked rather than erased.
    const ContentFacts& inside = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(inside.geometries.size(), 1U) << "the row is parked, not erased";
    (void)uploads.beginFrame(inside, timeline, retirement);
    EXPECT_EQ(uploads.live(), 2U) << "inside slots + 1 frames the uploads are still declared";

    // One completed frame past the window: the row is gone, so nothing declares the streams any more.
    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), retirement.slots()));
    const ContentFacts& after = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(after.geometries.size(), 0U);
    (void)uploads.beginFrame(after, timeline, retirement);
    EXPECT_EQ(uploads.live(), 0U);
    EXPECT_EQ(uploads.retired(), 2U);
}

TEST(ContentStoreTest, AStreamWhoseBufferMovedLeavesWithoutTheGeometryChanging)
{
    ContentStore store;
    auto         positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F }));
    auto geometry = vn::intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(positions);
    setPrimitiveIndices(
        *geometry,
        0U,
        vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);
    store.track(geometry);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), nullptr, nullptr) });

    StreamUploads       uploads;
    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(bindStreams(uploads, facts.geometries.front()), 2U);
    const std::uint64_t builds_before = store.builds();

    // The host refilled the positions buffer and announced it ON THE BUFFER; the geometry did not move.
    positions->setRevision(positions->revision() + 1U);
    const ContentFacts& moved = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_EQ(store.builds(), builds_before) << "a buffer announcement does not rebuild the geometry's row";
    (void)uploads.beginFrame(moved, timeline, retirement);

    EXPECT_EQ(uploads.live(), 1U) << "the positions entry's key moved; the index entry is untouched";
    EXPECT_EQ(uploads.retired(), 1U) << "the previous revision's upload is not what these bytes are";

    const vn::vsg::ChannelFacts& positions_channel = moved.geometries.front().channels.front();
    const auto                   again             = uploads.acquireVertex(positions_channel.streamKey(), positions_channel.scalars(), positions_channel.values);
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "the new bytes were never uploaded under this key";
    EXPECT_EQ(uploads.uploads(), 3U);
}

TEST(ContentStoreTest, TheDeclarationsFollowTheRowsAndOutliveThemForTheParkingWindow)
{
    ContentStore store;
    auto         positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F }));
    const auto make = [&positions]() {
        auto geometry = vn::intrusive_ptr<Geometry>(new Geometry());
        geometry->setPositions(positions);
        setPrimitiveIndices(
            *geometry,
            0U,
            vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
        geometry->setRevision(1U);
        return geometry;
    };
    auto left  = make();
    auto right = make();
    store.track(left);
    store.track(right);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(left.get(), left->revision(), nullptr, nullptr), command(right.get(), right->revision(), nullptr, nullptr) });

    const ContentFacts& both = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(both.geometries.size(), 2U);
    const void* buffer = both.geometries.front().channels.front().values.get();
    ASSERT_NE(buffer, nullptr);
    EXPECT_TRUE(both.declarations.declaresBuffer(buffer)) << "both rows declare the shared arena";
    EXPECT_FALSE(both.declarations.declaresBuffer(nullptr));

    // One geometry goes: the OTHER row still declares the buffer, so the index keeps it (the refcount, not a bit).
    left.reset();
    std::vector<vn::graphics::MaterialId> released_materials;
    ASSERT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 1U);
    EXPECT_TRUE(store.tablesFor(plan->frame, timeline, retirement).declarations.declaresBuffer(buffer))
        << "a shared buffer is declared while ANY row still names it";

    // The last declarer goes too; the parked row keeps the declaration for the window, then it is gone.
    right.reset();
    ASSERT_EQ(store.releaseAbandoned(timeline, retirement, released_materials), 1U);
    EXPECT_TRUE(store.tablesFor(plan->frame, timeline, retirement).declarations.declaresBuffer(buffer)) << "the row is parked, not erased";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), retirement.slots()));
    EXPECT_FALSE(store.tablesFor(plan->frame, timeline, retirement).declarations.declaresBuffer(buffer))
        << "with the last row erased, nothing declares the bytes any more";
}

TEST(ContentStoreTest, AnInstanceStreamIsDeclaredThroughItsPrimitiveAtTheRowsRevision)
{
    ContentStore store;
    auto         instances = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(16U, 0.0F)));
    auto         geometry  = quad();
    ASSERT_TRUE(geometry->setInstanceTransforms(0U, vn::BufferSlice<float>::whole(instances)));
    EXPECT_EQ(geometry->revision(), 1U) << "the instance setter does not describe the change; the host reports it";
    store.track(geometry);

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    const auto      plan = contentPlan({ command(geometry.get(), geometry->revision(), nullptr, nullptr) });

    const ContentFacts& facts = store.tablesFor(plan->frame, timeline, retirement);
    ASSERT_EQ(facts.geometries.size(), 1U);
    const vn::vsg::GeometryFacts& row = facts.geometries.front();
    ASSERT_EQ(row.primitives.size(), 1U);
    EXPECT_EQ(row.primitives.front().instances.get(), instances.get()) << "the row carries the draw's instance stream";
    EXPECT_TRUE(facts.declarations.declaresBuffer(instances.get())) << "and the declaration is read from that handle";

    // Swapping the slice WITHOUT reporting a new description: the row is the snapshot the live revision answered
    // with, so it still describes - and declares - the stream the draw used before (see the pitfall in the design doc).
    auto replacement = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(16U, 1.0F)));
    ASSERT_TRUE(geometry->setInstanceTransforms(0U, vn::BufferSlice<float>::whole(replacement)));
    const ContentFacts& unreported = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_TRUE(unreported.declarations.declaresBuffer(instances.get())) << "the row still describes the stream it was built from";
    EXPECT_FALSE(unreported.declarations.declaresBuffer(replacement.get())) << "the swapped-in stream is not declared until the description moves";

    // The host reports the new description (the SDK's contract, see Geometry::setInstanceTransforms): the live
    // revision declares what it draws, and the superseded row keeps declaring the old stream until its park comes due.
    geometry->bumpRevision();
    store.track(geometry); // the frame hands the new shape over
    const ContentFacts& reported = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_TRUE(reported.declarations.declaresBuffer(replacement.get())) << "the live revision declares what it draws";
    EXPECT_TRUE(reported.declarations.declaresBuffer(instances.get())) << "the superseded row is still answerable, so its stream stays declared";

    releaseUpTo(timeline, retirement, FrameTimeline::retirePoint(timeline.submittedFrame(), retirement.slots()));
    const ContentFacts& pruned = store.tablesFor(plan->frame, timeline, retirement);
    EXPECT_TRUE(pruned.declarations.declaresBuffer(replacement.get()));
    EXPECT_FALSE(pruned.declarations.declaresBuffer(instances.get())) << "with the old row erased, nothing declares the old stream";
}
