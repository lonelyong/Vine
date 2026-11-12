#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <vine/graphics/Geometry.hpp>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/intrusive_ptr.hpp>

#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/graphics/backend/StateRegistry.hpp>
#include <vine/vsg/internal/ContentDraw.hpp>
#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/ContentHalves.hpp>
#include <vine/vsg/internal/ContentSources.hpp>
#include <vine/vsg/internal/DrawNeeds.hpp>
#include <vine/vsg/internal/FactResult.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>

#include "TestGeometryBuilders.hpp"
#include "TestGeometryFacts.hpp"
#include "TestMaterialFacts.hpp"
#include "TestProgramFacts.hpp"

using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::backend::CompiledCommand;
using vn::graphics::backend::CompiledDraw;
using vn::graphics::backend::CompiledPass;
using vn::graphics::backend::DrawKind;
using vn::graphics::backend::FrameTimeline;
using vn::graphics::backend::PipelineKey;
using vn::graphics::backend::RenderPassCompatibility;
using vn::graphics::backend::RetirementQueue;
using vn::graphics::backend::StateRegistry;
using vn::graphics::backend::VariantPool;
using vn::graphics::Topology;
using vn::test::setPrimitiveIndices;
using vn::vsg::buildGeometryFacts;
using vn::vsg::buildMaterialFacts;
using vn::test::buildMaterialFacts; // the object-pointer overload a case writes (see TestMaterialFacts.hpp)
using vn::vsg::buildProgramFacts;
using vn::vsg::ChannelFacts;
using vn::vsg::ContentDeclarationIndexes;
using vn::vsg::ContentDeclarations;
using vn::vsg::ContentDraw;
using vn::vsg::ContentFacts;
using vn::vsg::ContentHalves;
using vn::vsg::ContentPass;
using vn::vsg::DrawNeeds;
using vn::vsg::drawNeeds;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;
using vn::vsg::LayoutDeclarations;
using vn::vsg::MaterialFacts;
using vn::vsg::ProgramFacts;
using vn::vsg::ProgramVariant;

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

/// @brief A content program whose fragment stage gates its sampler on `VN_TEXTURED` - one text, two ABIs.
/// The texts are written the way the engine's own stages are (see builtin_forward.vert) so they COMPILE: this
/// producer builds real layers.
constexpr const char* kGatedVertexSource = "#pragma import_defines (VN_TEXTURED, VN_TEXCOORD_UV)\n"
                                           "layout(location = 0) in vec3 position;\n"
                                           "#ifdef VN_TEXTURED\n"
                                           "layout(location = 8) in vec2 texcoord;\n"
                                           "#endif\n"
                                           "void main() { gl_Position = vec4(position, 1.0); }\n";

constexpr const char* kGatedFragmentSource = "#pragma import_defines (VN_TEXTURED)\n"
                                             "layout(location = 0) out vec4 outColor;\n"
                                             "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
                                             "{\n"
                                             "    vec4 base_color; vec4 emissive; vec4 params; uvec4 flags;\n"
                                             "} material;\n"
                                             "#ifdef VN_TEXTURED\n"
                                             "layout(set = 0, binding = 1) uniform sampler2D vn_baseColorMap;\n"
                                             "#endif\n"
                                             "void main() { outColor = material.base_color; }\n";

constexpr const char* kScreenFragmentSource = "#version 450\n"
                                              "layout(location = 0) in vec2 vn_uv;\n"
                                              "layout(location = 0) out vec4 outColor;\n"
                                              "void main() { outColor = vec4(vn_uv, 0.0, 1.0); }\n";

/// @brief A content program with both stages.
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

/// @brief What a test's tables are: the entries and the storage their spans point into, kept together.
///
/// The storage is ONE VECTOR PER ENTRY: a builder clears the storage it is given, so two entries sharing
/// one vector would leave the first entry's span reading the second's data (the spans alias the vector's
/// heap buffer, which a move preserves - so an outer vector is a stable home for them).
struct Tables {
    std::vector<ProgramFacts>                         programs;
    std::vector<GeometryFacts>                        geometries;
    std::vector<MaterialFacts>                        materials;
    std::vector<std::vector<ChannelFacts>>            channel_storage;   // One per geometry entry.
    std::vector<std::vector<vn::vsg::PrimitiveFacts>> primitive_storage; // One per geometry entry: its draws.
    std::vector<std::vector<std::byte>>               block_storage;     // One per material entry.
    LayoutDeclarations                                layouts;           // Layout -> live row count (see ContentDeclarations).
    ContentFacts                                      facts;

    /// @brief Publishes the spans AND the declaration indexes, the way ContentStore::tablesFor does.
    ///
    /// The half pin asks `declaresLayout`, so a hand-built table that only filled the spans would model a store
    /// that never published its index - every half would leave on the next sweep. This is the production shape.
    void publish()
    {
        layouts.clear();
        for (const GeometryFacts& geometry : geometries) {
            ++layouts[geometry.layout];
        }
        facts.programs     = programs;
        facts.geometries   = geometries;
        facts.materials    = materials;
        facts.declarations = ContentDeclarations{ ContentDeclarationIndexes{ .layouts = &layouts } };
    }
};

/// @brief A pass with the given draws (the spans are into the caller's vectors).
CompiledPass passWith(std::span<const CompiledDraw> draws, std::uint32_t color_attachments = 1U)
{
    CompiledPass pass;
    pass.draws             = draws;
    pass.color_attachments = color_attachments;
    return pass;
}

/// @brief A content command over @p geometry (id and revision, as one plan row keys them), @p program and @p material.
CompiledCommand commandOf(vn::graphics::ContentRef geometry, const ShaderProgram* program, std::uint64_t program_revision, vn::graphics::ContentRef material)
{
    CompiledCommand out;
    out.geometry = geometry;
    out.program  = program != nullptr ? vn::graphics::contentRefOf(program->id(), program_revision) : vn::graphics::ContentRef{};
    out.material = material;
    return out;
}

/** @brief The same command naming a material by the OBJECT a test holds: the plan carries the id and the revision. */
CompiledCommand commandOf(vn::graphics::ContentRef geometry, const ShaderProgram* program, std::uint64_t program_revision, const vn::graphics::MaterialBase* material)
{
    const vn::graphics::ContentRef ref =
        material != nullptr ? vn::graphics::ContentRef{ material->id().handle(), material->revision() } : vn::graphics::ContentRef{};
    return commandOf(geometry, program, program_revision, ref);
}

/** @brief The same command named by the OBJECT a test holds: a plan carries the geometry's id and the revision its
 *         shape is at (a `Geometry` inherits its id from `Node`), so the two halves make one ref here. */
CompiledCommand commandOf(const vn::graphics::Geometry* geometry, std::uint64_t geometry_revision, const ShaderProgram* program, std::uint64_t program_revision, vn::graphics::ContentRef material)
{
    const vn::graphics::NodeId identity = geometry != nullptr ? geometry->id() : vn::graphics::NodeId{};
    return commandOf(vn::graphics::contentRefOf(identity, geometry_revision), program, program_revision, material);
}

/** @brief Both identities named by the OBJECTS a case holds - the shape most cases write. */
CompiledCommand commandOf(const vn::graphics::Geometry* geometry, std::uint64_t geometry_revision, const ShaderProgram* program, std::uint64_t program_revision, const vn::graphics::MaterialBase* material)
{
    const vn::graphics::NodeId identity = geometry != nullptr ? geometry->id() : vn::graphics::NodeId{};
    return commandOf(vn::graphics::contentRefOf(identity, geometry_revision), program, program_revision, material);
}

} // namespace

TEST(ContentHalvesTest, OneHalfPerTupleThePassNames)
{
    const auto program  = contentProgram();
    const auto geometry = quad();
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);

    Tables tables;
    tables.programs.push_back(program_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    // ONE draw, TWO commands over the same tuple: the pass asks for one half, not two.
    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()),
                                                 commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(halves.halves(), 1U);
    EXPECT_EQ(halves.builds(), 1U);
    EXPECT_EQ(halves.refused(), 0U);

    EXPECT_EQ(entries[0].kind, DrawKind::Content);
    EXPECT_EQ(entries[0].program, vn::test::programRef(*program));
    EXPECT_EQ(entries[0].layout, geometry_facts.layout);
    EXPECT_EQ(entries[0].variant, ProgramVariant{});
    EXPECT_NE(entries[0].pipelines, nullptr) << "the half carries the compiled layer";
    EXPECT_NE(entries[0].draws, nullptr) << "and the recorder over it";
    EXPECT_EQ(entries[0].pipelines->kind(), DrawKind::Content);
}

TEST(ContentHalvesTest, ASteadyPassBuildsNothing)
{
    const auto program  = contentProgram();
    const auto geometry = quad();
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    const auto first = halves.halvesFor(pass, tables.facts, timeline, retirement);
    ASSERT_EQ(first.size(), 1U);
    const void* const   layer = first[0].pipelines;
    const std::uint64_t built = halves.builds();

    for (int frame = 0; frame < 3; ++frame) {
        const auto again = halves.halvesFor(pass, tables.facts, timeline, retirement);
        ASSERT_EQ(again.size(), 1U);
        EXPECT_EQ(again[0].pipelines, layer) << "the same half is served, not rebuilt";
        EXPECT_EQ(halves.builds(), built);
    }
    EXPECT_EQ(retirement.pending(), 0U);
}

TEST(ContentHalvesTest, TheHalvesEpisodeStateIsTheHalvesOwn)
{
    // THE REPORT OF A HALF SPANS FRAMES (and passes): the frame path builds a ContentPass - and its episode
    // state - per FRAME, so a sentence about a half that repeats while the condition holds would be said
    // again every frame (measured 2026-09-24 on the demo's `vn_shadow_map` warning). The entries now carry the
    // HALF's own state, and the half lives as long as the tables answer for it.
    const auto program  = contentProgram();
    const auto geometry = quad();
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass first_pass  = passWith(draws);
    const CompiledPass second_pass = passWith(draws);

    const auto frame_one = halves.halvesFor(first_pass, tables.facts, timeline, retirement);
    ASSERT_EQ(frame_one.size(), 1U);
    ASSERT_NE(frame_one[0].reported, nullptr) << "the half's own episode state, not the recorder's";
    ASSERT_NE(frame_one[0].shadow_reported, nullptr);
    EXPECT_NE(frame_one[0].reported, frame_one[0].shadow_reported) << "the two sentences have their own episodes: one must not silence the other";

    // Frame one says it: the episode is open, and the caller can see that much.
    EXPECT_TRUE(frame_one[0].reported->shouldReport());
    EXPECT_TRUE(frame_one[0].reported->reported());

    // Frame two: a NEW recorder (a new scope), the SAME half state - so the sentence is NOT said again.
    const auto frame_two = halves.halvesFor(first_pass, tables.facts, timeline, retirement);
    ASSERT_EQ(frame_two.size(), 1U);
    EXPECT_EQ(frame_two[0].reported, frame_one[0].reported) << "the state is the half's, not the frame's";
    EXPECT_FALSE(frame_two[0].reported->shouldReport()) << "the episode outlives the frame that opened it";

    // Another PASS drawing through the same half shares it: the sentence is about the half.
    const auto other_pass = halves.halvesFor(second_pass, tables.facts, timeline, retirement);
    ASSERT_EQ(other_pass.size(), 1U);
    EXPECT_EQ(other_pass[0].reported, frame_one[0].reported);
    EXPECT_FALSE(other_pass[0].reported->shouldReport());

    // And the caller decides when the episode ENDS: the half being served again re-arms it, and the next
    // failure is said again ("fixed then broken reports again" - the rule the diagnostics carry).
    frame_one[0].reported->rearm();
    const auto frame_three = halves.halvesFor(first_pass, tables.facts, timeline, retirement);
    ASSERT_EQ(frame_three.size(), 1U);
    EXPECT_TRUE(frame_three[0].reported->shouldReport()) << "a re-armed episode reports again";
    // The two sentences have their own episodes: what one has said does not silence the other (the same
    // half can be unservable AND be shaded by a pass whose map its text cannot read).
    EXPECT_TRUE(frame_three[0].shadow_reported->shouldReport()) << "the shadow sentence has its own episode";
    EXPECT_FALSE(frame_three[0].shadow_reported->shouldReport()) << "... and it, too, is said once";
}

TEST(ContentHalvesTest, TwoTextsOfOneProgramAreTwoHalves)
{
    const auto program  = contentProgram();
    const auto geometry = quad(true);
    const auto plain    = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));
    const auto texture  = vn::intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const auto textured = ::material(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F), texture);

    ProgramVariant textured_variant;
    textured_variant.textured = true;
    ProgramFacts textured_facts;
    ProgramFacts plain_facts;
    ASSERT_EQ(buildProgramFacts(*program, textured_variant, textured_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, plain_facts), FactMiss::None);

    Tables tables;
    tables.programs.push_back(textured_facts);
    tables.programs.push_back(plain_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts textured_material_facts;
    MaterialFacts plain_material_facts;
    ASSERT_EQ(buildMaterialFacts(textured.get(), textured->description(), 1U, textured_material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(textured_material_facts);
    ASSERT_EQ(buildMaterialFacts(plain.get(), plain->description(), 1U, plain_material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(plain_material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), textured.get()),
                                                 commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), plain.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    EXPECT_EQ(halves.refused(), 0U) << "both layers compile";
    // The rows are keyed by the revision of the VALUE they describe, and the plan names that same revision: the two
    // halves can only disagree about a material if those two numbers drift apart (see ContentFacts::MaterialFacts).
    EXPECT_EQ(findMaterial(tables.facts, textured_material_facts.material).miss, FactMiss::None) << "the textured row answers";
    EXPECT_EQ(findMaterial(tables.facts, plain_material_facts.material).miss, FactMiss::None) << "the plain row answers";
    EXPECT_EQ(textured_material_facts.material, (vn::graphics::ContentRef{ textured->id().handle(), textured->revision() })) << "the row is keyed by the value's revision";
    ASSERT_EQ(entries.size(), 2U) << "one program, two texts: the pass is served two halves";
    EXPECT_EQ(entries[0].variant, textured_variant);
    EXPECT_EQ(entries[1].variant, ProgramVariant{});
    EXPECT_NE(entries[0].pipelines, entries[1].pipelines);
    EXPECT_EQ(halves.halves(), 2U);
}

TEST(ContentHalvesTest, ThePassAttachmentCountIsPartOfTheHalf)
{
    const auto program  = contentProgram();
    const auto geometry = quad();
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };

    // A layer's blend state declares its attachment count, so a one-attachment layer is not the same compiled
    // object as a four-attachment one - the pass' shape is part of the identity.
    const CompiledPass one  = passWith(draws, 1U);
    const CompiledPass four = passWith(draws, 4U);
    EXPECT_EQ(halves.halvesFor(one, tables.facts, timeline, retirement).size(), 1U);
    EXPECT_EQ(halves.halvesFor(four, tables.facts, timeline, retirement).size(), 1U);
    EXPECT_EQ(halves.halves(), 2U);
}

TEST(ContentHalvesTest, ALayoutIsPartOfTheHalf)
{
    const auto program        = contentProgram();
    const auto plain_geometry = quad(false);
    const auto uv_geometry    = quad(true);
    const auto shaded         = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);

    GeometryFacts uv_geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*uv_geometry, uv_geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(uv_geometry_facts);
    GeometryFacts plain_geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*plain_geometry, plain_geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(plain_geometry_facts);
    ASSERT_NE(uv_geometry_facts.layout, plain_geometry_facts.layout);

    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{
        commandOf(uv_geometry.get(), uv_geometry->revision(), program.get(), program->revision(), shaded.get()),
        commandOf(plain_geometry.get(), plain_geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    ASSERT_EQ(entries.size(), 2U) << "the same program over two vertex layouts is two compiled halves";
    EXPECT_EQ(entries[0].layout, uv_geometry_facts.layout);
    EXPECT_EQ(entries[1].layout, plain_geometry_facts.layout);
}

TEST(ContentHalvesTest, AHalfIsPinnedByTheLayoutDeclarationAndLeavesWithIt)
{
    const auto program  = contentProgram();
    const auto geometry = quad(true);
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.publish();

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    ASSERT_EQ(halves.halvesFor(pass, tables.facts, timeline, retirement).size(), 1U);
    ASSERT_EQ(halves.halves(), 1U);

    // The program revision still answers, and the LAYOUT INDEX still says a live geometry feeds the half's layout:
    // the half stays even though this facts value carries no geometry rows at all. That is the point of the index -
    // the question is answered without walking the rows.
    ContentFacts program_and_layout;
    program_and_layout.programs      = tables.facts.programs;
    program_and_layout.program_names = tables.facts.program_names;
    program_and_layout.program_order = tables.facts.program_order;
    program_and_layout.declarations  = tables.facts.declarations;

    EXPECT_TRUE(halves.halvesFor(pass, program_and_layout, timeline, retirement).empty()) << "no geometry answers, so no draw is served";
    halves.sweep(program_and_layout, timeline, retirement); // the FRAME's step; this case drives no frame of its own
    EXPECT_EQ(halves.halves(), 1U) << "the layout declaration still pins the half";
    EXPECT_EQ(retirement.pending(), 0U) << "nothing left the table";

    // The last geometry of that layout is gone: the declaration index is empty, and the half leaves even though
    // the program revision still answers.
    LayoutDeclarations empty_layouts;
    ContentFacts       program_only;
    program_only.programs      = tables.facts.programs;
    program_only.program_names = tables.facts.program_names;
    program_only.program_order = tables.facts.program_order;
    program_only.declarations  = ContentDeclarations{ ContentDeclarationIndexes{ .layouts = &empty_layouts } };

    EXPECT_TRUE(halves.halvesFor(pass, program_only, timeline, retirement).empty());
    halves.sweep(program_only, timeline, retirement);
    EXPECT_EQ(halves.halves(), 0U) << "the half is pinned by program revision AND a live layout";
    EXPECT_EQ(retirement.pending(), 1U) << "the departed half is parked, not freed in place";
}

TEST(ContentHalvesTest, AFrameThatDrawsNothingStillRetiresWhatContentLeft)
{
    // RETIREMENT IS THE FRAME'S STEP, not a pass' (see ContentHalves::sweep): a frame whose passes issue nothing
    // never calls halvesFor, and the halves whose content left must still depart - otherwise a scene that is emptied
    // and then sits still would hold its pipelines until the session ends.
    const auto program  = contentProgram();
    const auto geometry = quad(true);
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.publish();

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    ASSERT_EQ(halves.halvesFor(pass, tables.facts, timeline, retirement).size(), 1U);
    ASSERT_EQ(halves.halves(), 1U);

    // The content is gone: no program row answers the revision, and no geometry row feeds the layout.
    ContentFacts empty;
    halves.sweep(empty, timeline, retirement); // no pass was walked at all - which is the point
    EXPECT_EQ(halves.halves(), 0U) << "a frame with nothing to draw still retires what content left";
    EXPECT_EQ(retirement.pending(), 1U) << "and it parks it, exactly as a drawing frame would";
}

TEST(ContentHalvesTest, AScreenDrawGetsAFullScreenHalf)
{
    const auto program = screenProgram();

    ProgramFacts screen_facts;
    ASSERT_EQ(vn::vsg::buildScreenProgramFacts(*program, screen_facts), FactMiss::None);

    Tables tables;
    tables.programs.push_back(screen_facts);
    tables.facts.programs = tables.programs;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    CompiledDraw screen_draw;
    screen_draw.kind    = DrawKind::Screen;
    screen_draw.program = vn::test::programRef(*program);
    const std::vector<CompiledDraw> draws{ screen_draw };
    const CompiledPass              pass = passWith(draws);

    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].kind, DrawKind::Screen);
    EXPECT_EQ(entries[0].pipelines->kind(), DrawKind::Screen);
}

TEST(ContentHalvesTest, AKeyTheTablesCannotAnswerProducesNoHalf)
{
    const auto program  = contentProgram();
    const auto geometry = quad();
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const ContentFacts                 empty{};
    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    EXPECT_TRUE(halves.halvesFor(pass, empty, timeline, retirement).empty()) << "a store cannot invent a half for an identity nobody answered for";
    EXPECT_EQ(halves.builds(), 0U);
}

TEST(ContentHalvesTest, AHalfWhoseKeyLeftTheTablesIsParked)
{
    const auto program  = contentProgram();
    const auto geometry = quad();
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts old_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, old_facts), FactMiss::None);
    const std::uint64_t old_revision = program->revision();

    Tables tables;
    tables.programs.push_back(old_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> old_commands{ commandOf(geometry.get(), geometry->revision(), program.get(), old_revision, shaded.get()) };
    const std::vector<CompiledDraw>    old_draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, old_commands }
    };
    const CompiledPass old_pass = passWith(old_draws);
    ASSERT_EQ(halves.halvesFor(old_pass, tables.facts, timeline, retirement).size(), 1U);
    ASSERT_EQ(halves.halves(), 1U);

    // The host edits the program (a single stage's source) and the tables are rebuilt for the new revision -
    // the OLD revision's entry is gone, exactly as api/ContentStore leaves it once its own window is past.
    ShaderStage patched = stage(ShaderStageType::Fragment, kGatedFragmentSource);
    patched.source      = vn::String(reinterpret_cast<const char8_t*>("#pragma import_defines (VN_TEXTURED)\n"
                                                                      "layout(location = 0) out vec4 outColor;\n"
                                                                      "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
                                                                      "{\n"
                                                                      "    vec4 base_color; vec4 emissive; vec4 params; uvec4 flags;\n"
                                                                      "} material;\n"
                                                                      "void main() { outColor = material.base_color; }\n"));
    ASSERT_TRUE(program->setStage(1U, patched));
    const std::uint64_t new_revision = program->revision();
    ASSERT_GT(new_revision, old_revision);

    ProgramFacts new_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, new_facts), FactMiss::None);
    Tables fresh;
    fresh.programs.push_back(new_facts);
    GeometryFacts fresh_geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, fresh_geometry_facts, fresh.channel_storage.emplace_back(), fresh.primitive_storage.emplace_back()),
              FactMiss::None);
    fresh.geometries.push_back(fresh_geometry_facts);
    MaterialFacts fresh_material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, fresh_material_facts, fresh.block_storage.emplace_back()), FactMiss::None);
    fresh.materials.push_back(fresh_material_facts);
    fresh.facts.programs   = fresh.programs;
    fresh.facts.geometries = fresh.geometries;
    fresh.facts.materials  = fresh.materials;

    const std::vector<CompiledCommand> new_commands{ commandOf(geometry.get(), geometry->revision(), program.get(), new_revision, shaded.get()) };
    const std::vector<CompiledDraw>    new_draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, new_commands }
    };
    const CompiledPass new_pass = passWith(new_draws);

    ASSERT_EQ(halves.halvesFor(new_pass, fresh.facts, timeline, retirement).size(), 1U);
    halves.sweep(fresh.facts, timeline, retirement); // the FRAME's step (see ContentHalves::sweep)
    EXPECT_EQ(halves.halves(), 1U) << "the live list holds the revision the tables answer";
    EXPECT_EQ(retirement.pending(), 1U) << "the half whose key left the tables was PARKED, not dropped: a"
                                           " recorded frame may still name it";
    const std::uint64_t builds = halves.builds();

    timeline.completeUpTo(FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    retirement.advance(timeline);
    EXPECT_EQ(retirement.pending(), 0U);
    EXPECT_EQ(halves.halves(), 1U);
    EXPECT_EQ(halves.builds(), builds) << "the sweep never rebuilds what it parks";
}

TEST(ContentHalvesTest, ARefusedLayerIsNotRetriedEveryFrame)
{
    // A program whose GLSL does not compile (a symbol nobody defines) still has FACTS - the ABI scan reads
    // declarations, not semantics - so the producer is asked for its half and the LAYER is refused. That
    // refusal is remembered: recompiling the same broken text every frame would put a shader compile in the
    // frame loop and say the same thing again.
    const auto program = vn::intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->addStage(stage(ShaderStageType::Vertex, kGatedVertexSource));
    program->addStage(stage(ShaderStageType::Fragment,
                            "layout(location = 0) out vec4 outColor;\n"
                            "void main() { outColor = vec4(no_such_symbol); }\n"));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);

    Tables tables;
    tables.programs.push_back(program_facts);
    const auto    geometry = quad();
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    const auto    shaded = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()) };
    const std::vector<CompiledDraw>    draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands }
    };
    const CompiledPass pass = passWith(draws);

    EXPECT_TRUE(halves.halvesFor(pass, tables.facts, timeline, retirement).empty()) << "the layer was refused, so no half is served";
    EXPECT_EQ(halves.refused(), 1U);
    EXPECT_EQ(halves.builds(), 1U);

    for (int frame = 0; frame < 3; ++frame) {
        EXPECT_TRUE(halves.halvesFor(pass, tables.facts, timeline, retirement).empty());
    }
    EXPECT_EQ(halves.builds(), 1U) << "a refused key is remembered, not retried";
    EXPECT_EQ(halves.refused(), 1U);
}

// 两个程序即使文本与 revision 完全相同，也必须是两个半边：身份是句柄（ShaderProgram::id()），不是地址，也不是
// revision。旧键是“地址 + revision”，因此需要一个 ProgramFacts::generation 来区分“同一地址上的另一次描述”；
// 句柄在释放时移动 generation，这个补丁因此消失。
TEST(ContentHalvesTest, AnotherProgramIdentityIsAnotherHalf)
{
    const auto first_program  = contentProgram();
    const auto second_program = contentProgram(); // same text, same revision, a DIFFERENT identity
    const auto geometry       = quad();
    const auto shaded         = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    Tables tables;
    for (const auto& program : { first_program, second_program }) {
        ProgramFacts program_facts;
        ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
        tables.programs.push_back(program_facts);
    }

    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), shaded->description(), 1U, material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> first_commands{
        commandOf(geometry.get(), geometry->revision(), first_program.get(), first_program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> first_draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, first_commands }
    };
    const auto first = halves.halvesFor(passWith(first_draws), tables.facts, timeline, retirement);
    ASSERT_EQ(first.size(), 1U);
    const void* const first_layer = first[0].pipelines;
    ASSERT_NE(first_layer, nullptr);
    ASSERT_EQ(halves.builds(), 1U);

    // A DIFFERENT program, at the same revision, drawing the same geometry with the same material: its half is
    // its own, because the identity is part of the key.
    const std::vector<CompiledCommand> second_commands{
        commandOf(geometry.get(), geometry->revision(), second_program.get(), second_program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> second_draws{
        CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, second_commands }
    };
    const auto second = halves.halvesFor(passWith(second_draws), tables.facts, timeline, retirement);
    ASSERT_EQ(second.size(), 1U);
    const void* const second_layer = second[0].pipelines;
    EXPECT_NE(second_layer, first_layer) << "another program identity is another half";
    EXPECT_EQ(halves.builds(), 2U);
    EXPECT_EQ(retirement.pending(), 0U) << "both programs are still answerable: nothing was parked";

    // Steady again: each identity keeps its own half, and nothing is rebuilt.
    for (int frame = 0; frame < 3; ++frame) {
        const auto again = halves.halvesFor(passWith(second_draws), tables.facts, timeline, retirement);
        ASSERT_EQ(again.size(), 1U);
        EXPECT_EQ(again[0].pipelines, second_layer);
    }
    const auto first_again = halves.halvesFor(passWith(first_draws), tables.facts, timeline, retirement);
    ASSERT_EQ(first_again.size(), 1U);
    EXPECT_EQ(first_again[0].pipelines, first_layer) << "the first program's half is still the one it built";
    EXPECT_EQ(halves.builds(), 2U);
}

TEST(ContentHalvesTest, TheReportedNeedsAreWhatTheFrameBuilds)
{
    // T32: what the warm-up path REPORTS and what the frame BUILDS are one value. The case spells the identities
    // BY HAND (no drawNeeds call feeds the expectation), then checks that the answer spells them and that the
    // frame's compiled set - the pool, filled by recording each drawable through the half its answer names - is
    // exactly that set. A backend that derived the tuple twice would pass a count comparison and fail this one.
    const auto program  = contentProgram();
    const auto geometry = quad(true);
    const auto plain    = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));
    const auto texture  = vn::intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const auto textured = ::material(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F), texture);

    ProgramVariant textured_variant;
    textured_variant.textured = true;
    ProgramFacts textured_facts;
    ProgramFacts plain_facts;
    ASSERT_EQ(buildProgramFacts(*program, textured_variant, textured_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, plain_facts), FactMiss::None);

    Tables tables;
    tables.programs.push_back(textured_facts);
    tables.programs.push_back(plain_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back(), tables.primitive_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts textured_material_facts;
    MaterialFacts plain_material_facts;
    ASSERT_EQ(buildMaterialFacts(textured.get(), textured->description(), 1U, textured_material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(textured_material_facts);
    ASSERT_EQ(buildMaterialFacts(plain.get(), plain->description(), 1U, plain_material_facts, tables.block_storage.emplace_back()), FactMiss::None);
    tables.materials.push_back(plain_material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    // THREE drawables, THREE identities: two texts of one program, and the plain one drawn as LINES (the
    // topology is baked into the half, so it is part of what the frame builds).
    std::vector<CompiledCommand> commands{ commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), textured.get()),
                                           commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), plain.get()),
                                           commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), plain.get()) };
    commands[2].dynamic.topology = Topology::Lines;
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              pass = passWith(draws);

    RenderPassCompatibility compatibility;
    compatibility.color_formats.push_back(vn::graphics::RenderTarget::ColorFormat::RGBA8);
    compatibility.device_color_formats.push_back(VK_FORMAT_R8G8B8A8_UNORM);
    compatibility.samples = 1U;

    // THE IDENTITY SPELLED BY HAND: this is the case's own answer, not the code's.
    const auto identity = [&](std::uint32_t variant_bits, Topology topology) {
        PipelineKey key;
        key.kind                = DrawKind::Content;
        key.program             = vn::test::programRef(*program);
        key.vertex_layout       = geometry_facts.layout;
        key.compatibility       = compatibility;
        key.depth_sampleable    = false;
        key.sampled_color_count = 0U;
        key.sampled_depth_count = 0U;
        key.variant             = variant_bits;
        key.topology            = topology;
        return key;
    };
    const std::vector<PipelineKey> expected{
        identity(textured_variant.bits(), Topology::Triangles),
        identity(ProgramVariant{}.bits(), Topology::Triangles),
        identity(ProgramVariant{}.bits(), Topology::Lines),
    };

    VariantPool     pool;
    ContentHalves   halves(pool);
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    // 1. THE ANSWER. A query: it compiles nothing, and it spells exactly the identities above - including the
    //    variant, which is a fact of the drawable rather than of the plan.
    std::vector<PipelineKey> answered;
    for (const CompiledCommand& command : commands) {
        const DrawNeeds needs = drawNeeds(pass, command, tables.facts);
        ASSERT_TRUE(needs.answerable());
        answered.push_back(needs.keyFor(compatibility));
    }
    EXPECT_EQ(pool.created(), 0U) << "answering what a frame needs builds nothing";
    ASSERT_EQ(answered.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_TRUE(answered[index] == expected[index]) << "answer " << index << " is the identity the case spelled";
    }

    // 2. THE HALVES THE FRAME IS SERVED BY: one per answer, and one per answer only.
    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    EXPECT_EQ(halves.refused(), 0U) << "all three texts compile";
    ASSERT_EQ(entries.size(), expected.size());
    EXPECT_EQ(halves.halves(), expected.size());

    // 3. THE FRAME'S BUILD: every drawable recorded through the half its own answer names, with the key the
    //    answer spells. The pool is what the recorder asked the identity arithmetic for, so its contents ARE the
    //    frame's compiled set.
    StateRegistry registry(pool);
    for (const CompiledCommand& command : commands) {
        const DrawNeeds                  needs = drawNeeds(pass, command, tables.facts);
        const ContentPass::Scope::Entry* half  = nullptr;
        for (const ContentPass::Scope::Entry& candidate : entries) {
            if (candidate.program == needs.program && candidate.layout == needs.layout && candidate.topology == needs.topology &&
                candidate.variant == needs.variant)
            {
                half = &candidate;
                break;
            }
        }
        ASSERT_NE(half, nullptr) << "the halves cover exactly the answers";
        ContentDraw::Draw draw;
        draw.key               = needs.keyFor(compatibility);
        draw.vertex_count      = geometry_facts.vertex_count;
        draw.viewport          = vn::vsg::ViewportRect{ 0.0F, 0.0F, 64.0F, 64.0F };
        draw.color_attachments = pass.color_attachments;
        ASSERT_NE(half->draws->record(registry, draw), nullptr) << "the frame builds the identity the answer named";
    }
    EXPECT_EQ(pool.created(), static_cast<std::uint64_t>(expected.size())) << "one compiled identity per answer, and no other";

    // ... and the set the pool holds IS the answered set, field for field: an identity the frame invented would be
    // a key no answer names, and the created() count above would not show it.
    std::vector<PipelineKey> built;
    for (std::uint64_t id = 1U; built.size() < pool.variants(); ++id) {
        if (const PipelineKey* key = pool.keyOf(id)) {
            built.push_back(*key);
        }
    }
    ASSERT_EQ(built.size(), expected.size()) << "ids are handed out from 1 and never reused (see VariantPool)";
    for (const PipelineKey& key : expected) {
        EXPECT_NE(std::find(built.begin(), built.end(), key), built.end());
    }
}
