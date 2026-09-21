/**
 * @brief Building the PROGRAM and MATERIAL entries of the content tables, from the SDK objects
 * (see `.ai/design/vsg-reimplementation.md` §11.17, `api/ContentSources.hpp`).
 *
 * Device-free by construction.
 *
 * The two rules these cases pin are the two the backend could otherwise get wrong silently:
 *
 *   * a program that is NOT exactly one vertex plus one fragment stage (or whose stages name different entry
 *     points) cannot become ONE content pipeline - compiling "the first one of each kind" runs a function the
 *     host never named, and nothing on screen says so;
 *   * content WITHOUT a material still draws, because the ABI's default-constructed block IS the default
 *     material: that is an entry with a null identity, not a lookup miss.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentSources.hpp>

using vine::graphics::Material;
using vine::graphics::ShaderProgram;
using vine::graphics::ShaderStage;
using vine::graphics::ShaderStageType;
using vine::vsg::buildMaterialFacts;
using vine::vsg::buildProgramFacts;
using vine::vsg::ContentFacts;
using vine::vsg::FactMiss;
using vine::vsg::findMaterial;
using vine::vsg::MaterialFacts;
using vine::vsg::ProgramFacts;

namespace
{

/// @brief A stage of @p type, with the given source and entry point (the ASCII form the SDK stores as UTF-8).
ShaderStage stage(ShaderStageType type, const char* source, const char* entry = "main")
{
    ShaderStage out;
    out.type       = type;
    out.source     = vine::String(reinterpret_cast<const char8_t*>(source));
    out.entryPoint = vine::String(reinterpret_cast<const char8_t*>(entry));
    return out;
}

constexpr const char* kVertexSource   = "#version 450\nvoid main() { gl_Position = vec4(0.0); }\n";
constexpr const char* kFragmentSource = "#version 450\nvoid main() {}\n";

}  // namespace

TEST(ContentSourcesTest, AProgramBecomesItsTwoStagesAndTheirEntryPoint)
{
    ShaderProgram program;
    program.addStage(stage(ShaderStageType::Vertex, kVertexSource, "render"));
    program.addStage(stage(ShaderStageType::Fragment, kFragmentSource, "render"));
    const std::uint64_t revision = program.revision();
    ASSERT_NE(revision, 0U);  // adding stages bumps the revision: the identity the plan will carry

    ProgramFacts facts;
    ASSERT_EQ(buildProgramFacts(program, facts), FactMiss::None);

    EXPECT_EQ(facts.program, &program);
    EXPECT_EQ(facts.revision, revision);
    EXPECT_EQ(facts.shaders.vertex, kVertexSource);
    EXPECT_EQ(facts.shaders.fragment, kFragmentSource);
    EXPECT_EQ(facts.shaders.entry, "render");
}

TEST(ContentSourcesTest, AProgramThatIsNotExactlyTwoGraphicsStagesCannotBeOneContentPipeline)
{
    ProgramFacts facts;

    ShaderProgram vertex_only;
    vertex_only.addStage(stage(ShaderStageType::Vertex, kVertexSource));
    EXPECT_EQ(buildProgramFacts(vertex_only, facts), FactMiss::Malformed);

    ShaderProgram fragment_only;
    fragment_only.addStage(stage(ShaderStageType::Fragment, kFragmentSource));
    EXPECT_EQ(buildProgramFacts(fragment_only, facts), FactMiss::Malformed);

    // Two fragment stages: "the first one wins" would compile a program the host did not write.
    ShaderProgram two_fragments;
    two_fragments.addStage(stage(ShaderStageType::Vertex, kVertexSource));
    two_fragments.addStage(stage(ShaderStageType::Fragment, kFragmentSource));
    two_fragments.addStage(stage(ShaderStageType::Fragment, kFragmentSource));
    EXPECT_EQ(buildProgramFacts(two_fragments, facts), FactMiss::Malformed);

    // A compute stage is not content.
    ShaderProgram with_compute;
    with_compute.addStage(stage(ShaderStageType::Vertex, kVertexSource));
    with_compute.addStage(stage(ShaderStageType::Fragment, kFragmentSource));
    with_compute.addStage(stage(ShaderStageType::Compute, kVertexSource));
    EXPECT_EQ(buildProgramFacts(with_compute, facts), FactMiss::Malformed);

    // An empty stage is not a program.
    ShaderProgram empty_source;
    empty_source.addStage(stage(ShaderStageType::Vertex, ""));
    empty_source.addStage(stage(ShaderStageType::Fragment, kFragmentSource));
    EXPECT_EQ(buildProgramFacts(empty_source, facts), FactMiss::Malformed);

    // One entry point serves both stages, so stages that disagree about it are not expressible.
    ShaderProgram split_entry;
    split_entry.addStage(stage(ShaderStageType::Vertex, kVertexSource, "main"));
    split_entry.addStage(stage(ShaderStageType::Fragment, kFragmentSource, "other"));
    EXPECT_EQ(buildProgramFacts(split_entry, facts), FactMiss::Malformed);

    ShaderProgram no_stages;
    EXPECT_EQ(buildProgramFacts(no_stages, facts), FactMiss::Unknown);
}

TEST(ContentSourcesTest, AMaterialBecomesItsAbiBlockFieldForField)
{
    Material material;
    material.setDiffuse(vine::Colorf(0.1F, 0.2F, 0.3F, 0.4F));
    material.setSpecular(vine::Colorf(0.5F, 0.6F, 0.7F, 1.0F));
    material.setAmbient(vine::Colorf(0.05F, 0.06F, 0.07F, 1.0F));
    material.setShininess(42.0F);

    MaterialFacts            facts;
    std::vector<std::byte>   storage;
    ASSERT_EQ(buildMaterialFacts(&material, 9U, facts, storage), FactMiss::None);

    EXPECT_EQ(facts.material, &material);
    EXPECT_EQ(facts.revision, 9U);
    EXPECT_EQ(facts.block.size(), sizeof(vine::graphics::VineMaterialBlock));

    vine::graphics::VineMaterialBlock block;
    ASSERT_GE(facts.block.size(), sizeof(block));
    std::memcpy(&block, facts.block.data(), sizeof(block));
    EXPECT_FLOAT_EQ(block.diffuse[0], 0.1F);
    EXPECT_FLOAT_EQ(block.diffuse[3], 0.4F);
    EXPECT_FLOAT_EQ(block.specular[1], 0.6F);
    EXPECT_FLOAT_EQ(block.ambient[2], 0.07F);
    EXPECT_FLOAT_EQ(block.shininess, 42.0F);
}

TEST(ContentSourcesTest, NoMaterialIsTheDefaultMaterialNotAMiss)
{
    MaterialFacts          facts;
    std::vector<std::byte> storage;
    ASSERT_EQ(buildMaterialFacts(nullptr, 0U, facts, storage), FactMiss::None);

    // The entry's identity is null, which is what content without a material looks up - and the bytes are the
    // ABI's own defaults (the existing implementation hands the same block to a drawable that names none).
    EXPECT_EQ(facts.material, nullptr);
    EXPECT_EQ(facts.block.size(), sizeof(vine::graphics::VineMaterialBlock));

    vine::graphics::VineMaterialBlock block{};
    std::memcpy(&block, facts.block.data(), sizeof(block));
    const vine::graphics::VineMaterialBlock defaults{};

    // The ABI's own comparison - member-wise, on purpose: the block's members are the payload, and its tail
    // padding (shininess ends at 52 of 64 bytes) is not part of it. Comparing the BYTES would report a change
    // for a material that did not change, which is why the ABI provides `operator==` rather than a hash.
    EXPECT_TRUE(block == defaults);
    EXPECT_EQ(block.diffuse[3], 0.0F);
    EXPECT_EQ(block.shininess, 32.0F) << "the one non-zero default the ABI states";

    // Building the same entry twice gives the same MEMBERS (the table's identity for it is the pair below).
    std::vector<std::byte> again_storage;
    MaterialFacts          again;
    ASSERT_EQ(buildMaterialFacts(nullptr, 0U, again, again_storage), FactMiss::None);
    vine::graphics::VineMaterialBlock again_block{};
    std::memcpy(&again_block, again.block.data(), sizeof(again_block));
    EXPECT_TRUE(again_block == block);

    // A table that carries the default entry answers for such content...
    const MaterialFacts entries[] = { facts };
    ContentFacts        table;
    table.materials = entries;
    const auto found = findMaterial(table, nullptr);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.entry->material, nullptr);

    // ...and a table that does not says Unknown, leaving the decision where it belongs.
    MaterialFacts  without_default{ &facts, 0U, storage };
    const MaterialFacts other[] = { without_default };
    ContentFacts        table_without;
    table_without.materials = other;
    EXPECT_EQ(findMaterial(table_without, nullptr).miss, FactMiss::Unknown);
}
