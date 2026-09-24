#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/intrusive_ptr.hpp>

#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentHalves.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/FactResult.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
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
using vn::vsg::buildGeometryFacts;
using vn::vsg::buildMaterialFacts;
using vn::vsg::buildProgramFacts;
using vn::vsg::ChannelFacts;
using vn::vsg::ContentFacts;
using vn::vsg::ContentHalves;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;
using vn::vsg::MaterialFacts;
using vn::vsg::ProgramFacts;
using vn::vsg::ProgramVariant;
using vn::vsg::core::CompiledCommand;
using vn::vsg::core::CompiledDraw;
using vn::vsg::core::CompiledPass;
using vn::vsg::core::DrawKind;
using vn::vsg::core::FrameTimeline;
using vn::vsg::core::ProgramRef;
using vn::vsg::core::RetirementQueue;
using vn::vsg::core::VariantPool;

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

/// @brief A content program whose fragment stage gates its sampler on `VINE_DIFFUSE_MAP` - one text, two ABIs.
/// The texts are written the way the engine's own stages are (see builtin_forward.vert) so they COMPILE: this
/// producer builds real layers.
constexpr const char* kGatedVertexSource =
    "#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV)\n"
    "layout(location = 0) in vec3 position;\n"
    "#ifdef VINE_DIFFUSE_MAP\n"
    "layout(location = 8) in vec2 texcoord;\n"
    "#endif\n"
    "void main() { gl_Position = vec4(position, 1.0); }\n";

constexpr const char* kGatedFragmentSource =
    "#pragma import_defines (VINE_DIFFUSE_MAP)\n"
    "layout(location = 0) out vec4 outColor;\n"
    "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
    "{\n"
    "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
    "} material;\n"
    "#ifdef VINE_DIFFUSE_MAP\n"
    "layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
    "#endif\n"
    "void main() { outColor = material.diffuse; }\n";

constexpr const char* kScreenFragmentSource =
    "#version 450\n"
    "layout(location = 0) in vec2 vine_uv;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() { outColor = vec4(vine_uv, 0.0, 1.0); }\n";

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
vn::intrusive_ptr<Material> material(vn::Colorf diffuse, vn::intrusive_ptr<Texture2D> texture = {})
{
    const auto out = vn::intrusive_ptr<Material>(new Material());
    out->setDiffuse(diffuse);
    if (texture != nullptr)
    {
        out->setTexture(texture);
    }
    return out;
}

/// @brief What a test's tables are: the entries and the storage their spans point into, kept together.
///
/// The storage is ONE VECTOR PER ENTRY: a builder clears the storage it is given, so two entries sharing
/// one vector would leave the first entry's span reading the second's data (the spans alias the vector's
/// heap buffer, which a move preserves - so an outer vector is a stable home for them).
struct Tables
{
    std::vector<ProgramFacts>              programs;
    std::vector<GeometryFacts>             geometries;
    std::vector<MaterialFacts>             materials;
    std::vector<std::vector<ChannelFacts>> channel_storage;  ///< One per geometry entry.
    std::vector<std::vector<std::byte>>    block_storage;    ///< One per material entry.
    ContentFacts                           facts;
};

/// @brief A pass with the given draws (the spans are into the caller's vectors).
CompiledPass passWith(std::span<const CompiledDraw> draws, std::uint32_t color_attachments = 1U)
{
    CompiledPass pass;
    pass.draws              = draws;
    pass.color_attachments  = color_attachments;
    return pass;
}

/// @brief A content command over @p geometry, @p program and @p material.
CompiledCommand commandOf(const void* geometry, std::uint64_t geometry_revision, const void* program,
                          std::uint64_t program_revision, const void* material)
{
    CompiledCommand out;
    out.geometry          = geometry;
    out.geometry_revision = geometry_revision;
    out.program           = ProgramRef{ program, program_revision };
    out.material          = material;
    return out;
}

}  // namespace

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
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    // ONE draw, TWO commands over the same tuple: the pass asks for one half, not two.
    const std::vector<CompiledCommand> commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get()),
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              pass = passWith(draws);

    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(halves.halves(), 1U);
    EXPECT_EQ(halves.builds(), 1U);
    EXPECT_EQ(halves.refused(), 0U);

    EXPECT_EQ(entries[0].kind, DrawKind::Content);
    EXPECT_EQ(entries[0].program, program.get());
    EXPECT_EQ(entries[0].revision, program->revision());
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
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              pass = passWith(draws);

    const auto first = halves.halvesFor(pass, tables.facts, timeline, retirement);
    ASSERT_EQ(first.size(), 1U);
    const void* const layer = first[0].pipelines;
    const std::uint64_t built = halves.builds();

    for (int frame = 0; frame < 3; ++frame)
    {
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
    // again every frame (measured 2026-09-24 on the demo's `shadow_map` warning). The entries now carry the
    // HALF's own state, and the half lives as long as the tables answer for it.
    const auto program  = contentProgram();
    const auto geometry = quad();
    const auto shaded   = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool    pool;
    ContentHalves  halves(pool);
    FrameTimeline  timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              first_pass  = passWith(draws);
    const CompiledPass              second_pass = passWith(draws);

    const auto frame_one = halves.halvesFor(first_pass, tables.facts, timeline, retirement);
    ASSERT_EQ(frame_one.size(), 1U);
    ASSERT_NE(frame_one[0].reported, nullptr) << "the half's own episode state, not the recorder's";
    ASSERT_NE(frame_one[0].shadow_reported, nullptr);
    EXPECT_NE(frame_one[0].reported, frame_one[0].shadow_reported)
        << "the two sentences have their own episodes: one must not silence the other";

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
    textured_variant.diffuse_map = true;
    ProgramFacts textured_facts;
    ProgramFacts plain_facts;
    ASSERT_EQ(buildProgramFacts(*program, textured_variant, textured_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, plain_facts), FactMiss::None);

    Tables tables;
    tables.programs.push_back(textured_facts);
    tables.programs.push_back(plain_facts);
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts textured_material_facts;
    MaterialFacts plain_material_facts;
    ASSERT_EQ(buildMaterialFacts(textured.get(), 1U, textured_material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(textured_material_facts);
    ASSERT_EQ(buildMaterialFacts(plain.get(), 1U, plain_material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(plain_material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), textured.get()),
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), plain.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              pass = passWith(draws);

    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    EXPECT_EQ(halves.refused(), 0U) << "both layers compile";
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
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };

    // A layer's blend state declares its attachment count, so a one-attachment layer is not the same compiled
    // object as a four-attachment one - the pass' shape is part of the identity.
    const CompiledPass one   = passWith(draws, 1U);
    const CompiledPass four  = passWith(draws, 4U);
    EXPECT_EQ(halves.halvesFor(one, tables.facts, timeline, retirement).size(), 1U);
    EXPECT_EQ(halves.halvesFor(four, tables.facts, timeline, retirement).size(), 1U);
    EXPECT_EQ(halves.halves(), 2U);
}

TEST(ContentHalvesTest, ALayoutIsPartOfTheHalf)
{
    const auto program = contentProgram();
    const auto plain_geometry = quad(false);
    const auto uv_geometry    = quad(true);
    const auto shaded = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, program_facts), FactMiss::None);
    Tables tables;
    tables.programs.push_back(program_facts);

    GeometryFacts uv_geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*uv_geometry, uv_geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(uv_geometry_facts);
    GeometryFacts plain_geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*plain_geometry, plain_geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(plain_geometry_facts);
    ASSERT_NE(uv_geometry_facts.layout, plain_geometry_facts.layout);

    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{
        commandOf(uv_geometry.get(), uv_geometry->revision(), program.get(), program->revision(), shaded.get()),
        commandOf(plain_geometry.get(), plain_geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              pass = passWith(draws);

    const auto entries = halves.halvesFor(pass, tables.facts, timeline, retirement);
    ASSERT_EQ(entries.size(), 2U) << "the same program over two vertex layouts is two compiled halves";
    EXPECT_EQ(entries[0].layout, uv_geometry_facts.layout);
    EXPECT_EQ(entries[1].layout, plain_geometry_facts.layout);
}

TEST(ContentHalvesTest, AScreenDrawGetsAFullScreenHalf)
{
    const auto program = screenProgram();

    ProgramFacts screen_facts;
    ASSERT_EQ(vn::vsg::buildScreenProgramFacts(*program, screen_facts), FactMiss::None);

    Tables tables;
    tables.programs.push_back(screen_facts);
    tables.facts.programs = tables.programs;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    CompiledDraw screen_draw;
    screen_draw.kind    = DrawKind::Screen;
    screen_draw.program = ProgramRef{ program.get(), program->revision() };
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

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    const ContentFacts empty{};
    const std::vector<CompiledCommand> commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              pass = passWith(draws);

    EXPECT_TRUE(halves.halvesFor(pass, empty, timeline, retirement).empty())
        << "a store cannot invent a half for an identity nobody answered for";
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
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back()),
              FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> old_commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), old_revision, shaded.get())
    };
    const std::vector<CompiledDraw> old_draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, old_commands } };
    const CompiledPass              old_pass = passWith(old_draws);
    ASSERT_EQ(halves.halvesFor(old_pass, tables.facts, timeline, retirement).size(), 1U);
    ASSERT_EQ(halves.halves(), 1U);

    // The host edits the program (a single stage's source) and the tables are rebuilt for the new revision -
    // the OLD revision's entry is gone, exactly as api/ContentStore leaves it once its own window is past.
    ShaderStage patched = stage(ShaderStageType::Fragment, kGatedFragmentSource);
    patched.source      = vn::String(reinterpret_cast<const char8_t*>(
        "#pragma import_defines (VINE_DIFFUSE_MAP)\n"
        "layout(location = 0) out vec4 outColor;\n"
        "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
        "{\n"
        "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
        "} material;\n"
        "void main() { outColor = material.diffuse; }\n"));
    ASSERT_TRUE(program->setStage(1U, patched));
    const std::uint64_t new_revision = program->revision();
    ASSERT_GT(new_revision, old_revision);

    ProgramFacts new_facts;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, new_facts), FactMiss::None);
    Tables fresh;
    fresh.programs.push_back(new_facts);
    GeometryFacts fresh_geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, fresh_geometry_facts, fresh.channel_storage.emplace_back()),
              FactMiss::None);
    fresh.geometries.push_back(fresh_geometry_facts);
    MaterialFacts fresh_material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, fresh_material_facts, fresh.block_storage.emplace_back()),
              FactMiss::None);
    fresh.materials.push_back(fresh_material_facts);
    fresh.facts.programs   = fresh.programs;
    fresh.facts.geometries = fresh.geometries;
    fresh.facts.materials  = fresh.materials;

    const std::vector<CompiledCommand> new_commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), new_revision, shaded.get())
    };
    const std::vector<CompiledDraw> new_draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, new_commands } };
    const CompiledPass              new_pass = passWith(new_draws);

    ASSERT_EQ(halves.halvesFor(new_pass, fresh.facts, timeline, retirement).size(), 1U);
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
    const auto geometry = quad();
    GeometryFacts geometry_facts;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, tables.channel_storage.emplace_back()), FactMiss::None);
    tables.geometries.push_back(geometry_facts);
    const auto shaded = ::material(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));
    MaterialFacts material_facts;
    ASSERT_EQ(buildMaterialFacts(shaded.get(), 1U, material_facts, tables.block_storage.emplace_back()),
              FactMiss::None);
    tables.materials.push_back(material_facts);
    tables.facts.programs   = tables.programs;
    tables.facts.geometries = tables.geometries;
    tables.facts.materials  = tables.materials;

    VariantPool   pool;
    ContentHalves halves(pool);
    FrameTimeline timeline;
    RetirementQueue retirement(1U);

    const std::vector<CompiledCommand> commands{
        commandOf(geometry.get(), geometry->revision(), program.get(), program->revision(), shaded.get())
    };
    const std::vector<CompiledDraw> draws{ CompiledDraw{ DrawKind::Content, {}, {}, {}, {}, {}, {}, commands } };
    const CompiledPass              pass = passWith(draws);

    EXPECT_TRUE(halves.halvesFor(pass, tables.facts, timeline, retirement).empty())
        << "the layer was refused, so no half is served";
    EXPECT_EQ(halves.refused(), 1U);
    EXPECT_EQ(halves.builds(), 1U);

    for (int frame = 0; frame < 3; ++frame)
    {
        EXPECT_TRUE(halves.halvesFor(pass, tables.facts, timeline, retirement).empty());
    }
    EXPECT_EQ(halves.builds(), 1U) << "a refused key is remembered, not retried";
    EXPECT_EQ(halves.refused(), 1U);
}
