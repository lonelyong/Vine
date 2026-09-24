/**
 * @brief Reading a program's declared bindings out of its text (see `.ai/design/vsg-reimplementation.md`
 * §11.16ae, `api/ProgramAbi.hpp`).
 *
 * Device-free by construction: the inputs are GLSL texts, and the engine's own programs are built in
 * process by `BuiltinShaders` - so these cases pin the REAL programs the scene bridge will serve, item by
 * item, rather than a paraphrase of them:
 *
 *   * the forward content program declares the material at set 0 / binding 0, the optional diffuse map at 1,
 *     the lights at 2 and - through the SDK's shadow insertion - the map at 3 and its block at 4, with the
 *     per-drawable block in set 1 and a 128-byte vertex push (the arrangement the existing backend serves,
 *     which is exactly the one a rewrite has to be able to reproduce);
 *   * the full-screen programs bind an attachment by BINDING NUMBER (screenCopyProgram(N)), and the
 *     shadowed deferred lighting program's pair follows the four G-buffer sources at 5 and 6;
 *   * a declaration inside a branch the variant does not take is not a fact, and a variant whose own text
 *     says `#error` is refused rather than half-read;
 *   * a role is the block's TYPE NAME (`VineViewBlock` ... - the L1 name, see graphics-shader.md §11.3), not
 *     the position it happens to sit at: that is what lets the rewrite's own arrangement and the engine's
 *     shipped one be served by one mechanism;
 *   * a text whose two stages disagree about one binding, or that carries a conditional this scan does not
 *     read, is refused: a binding guessed wrong is a shader reading memory nobody wrote.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/LightBlock.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>

using vn::graphics::ShaderProgram;
using vn::vsg::AbiBinding;
using vn::vsg::AbiBlockRole;
using vn::vsg::AbiDescriptorKind;
using vn::vsg::AbiStage;
using vn::vsg::buildProgramFacts;
using vn::vsg::buildScreenProgramFacts;
using vn::vsg::FactMiss;
using vn::vsg::ProgramAbi;
using vn::vsg::ProgramFacts;
using vn::vsg::scanProgramAbi;

namespace
{

constexpr std::uint32_t kVertex   = static_cast<std::uint32_t>(AbiStage::Vertex);
constexpr std::uint32_t kFragment = static_cast<std::uint32_t>(AbiStage::Fragment);

/// @brief Scans one SDK program's two stage texts for @p defines (the entry the tables carry).
ProgramAbi scan(const ShaderProgram& program, std::vector<std::string_view> defines = {})
{
    ProgramFacts facts;
    EXPECT_EQ(buildProgramFacts(program, facts), FactMiss::None) << "the program must be describable";
    ProgramAbi abi;
    EXPECT_EQ(scanProgramAbi(facts.shaders.vertex, facts.shaders.fragment, defines, abi), FactMiss::None);
    return abi;
}

/// @brief Scans a FULL-SCREEN program's entry (the engine's triangle + the program's fragment stage).
ProgramAbi scanScreen(const ShaderProgram& program, std::vector<std::string_view> defines = {})
{
    ProgramFacts facts;
    EXPECT_EQ(buildScreenProgramFacts(program, facts), FactMiss::None) << "the program must be describable";
    ProgramAbi abi;
    EXPECT_EQ(scanProgramAbi(facts.shaders.vertex, facts.shaders.fragment, defines, abi), FactMiss::None);
    return abi;
}

/// @brief Gets the binding at (set, binding), or null when the text declares none there.
const AbiBinding* at(const ProgramAbi& abi, std::uint32_t set, std::uint32_t binding)
{
    for (const AbiBinding& entry : abi.bindings)
    {
        if (entry.set == set && entry.binding == binding)
        {
            return &entry;
        }
    }
    return nullptr;
}

/// @brief Asserts a uniform block at (set, binding): its role, its stage, its std140 size and its layout.
void expectBlock(const ProgramAbi& abi, std::uint32_t set, std::uint32_t binding, AbiBlockRole role,
                 std::uint32_t stages, std::uint32_t size)
{
    const AbiBinding* entry = at(abi, set, binding);
    ASSERT_NE(entry, nullptr) << "set " << set << " binding " << binding;
    EXPECT_EQ(entry->kind, AbiDescriptorKind::UniformBlock);
    EXPECT_EQ(entry->role, role);
    EXPECT_EQ(entry->stages, stages);
    EXPECT_EQ(entry->layout, vn::vsg::AbiBlockLayout::Std140);
    EXPECT_EQ(entry->block_size, size);
}

/// @brief Asserts a sampler at (set, binding): its kind, its stage and its declared name.
void expectSampler(const ProgramAbi& abi, std::uint32_t set, std::uint32_t binding, AbiDescriptorKind kind,
                   std::uint32_t stages, const char* name)
{
    const AbiBinding* entry = at(abi, set, binding);
    ASSERT_NE(entry, nullptr) << "set " << set << " binding " << binding;
    EXPECT_EQ(entry->kind, kind);
    EXPECT_EQ(entry->role, AbiBlockRole::NotABlock);
    EXPECT_EQ(entry->stages, stages);
    EXPECT_EQ(entry->name, name);
}

}  // namespace

TEST(ProgramAbiTest, TheForwardProgramDeclaresTheContentAbiTheExistingBackendServes)
{
    const ProgramAbi abi = scan(*vn::graphics::forwardProgram());

    ASSERT_EQ(abi.bindings.size(), 5U);
    expectBlock(abi, 0U, 0U, AbiBlockRole::Material, kFragment, sizeof(vn::graphics::VineMaterialBlock));
    expectBlock(abi, 0U, 2U, AbiBlockRole::Lights, kFragment, sizeof(vn::vsg::VineLightsBlock));
    expectSampler(abi, 0U, 3U, AbiDescriptorKind::Sampler2D, kFragment, "shadow_map");
    expectBlock(abi, 0U, 4U, AbiBlockRole::ShadowBlock, kFragment, sizeof(vn::graphics::VineShadowBlock));
    expectBlock(abi, 1U, 0U, AbiBlockRole::Draw, kFragment, sizeof(vn::graphics::VineDrawBlock));

    // The texcoord slot's sampler is NOT declared by this variant: that is the fact the serving layer will
    // read to decide whether a pipeline needs the binding at all.
    EXPECT_EQ(at(abi, 0U, 1U), nullptr);

    // One push block: the two camera matrices the vertex stage reads, which is the whole 128-byte budget.
    // The MEMBERS are facts too, and they are what the serving layer fills BY NAME: `projection` first, then
    // `modelView`, each a mat4 (see api/ContentPush) - a push whose bytes were assembled by position would
    // swap the two the moment a program declared them in the other order.
    ASSERT_EQ(abi.pushes.size(), 1U);
    EXPECT_EQ(abi.pushes[0].offset, 0U);
    EXPECT_EQ(abi.pushes[0].size, 128U);
    EXPECT_EQ(abi.pushes[0].stages, kVertex);
    EXPECT_EQ(abi.pushes[0].type_name, "PushConstants");
    ASSERT_EQ(abi.pushes[0].members.size(), 2U);
    EXPECT_EQ(abi.pushes[0].members[0].name, "projection");
    EXPECT_EQ(abi.pushes[0].members[0].offset, 0U);
    EXPECT_EQ(abi.pushes[0].members[0].size, 64U);
    EXPECT_EQ(abi.pushes[0].members[1].name, "modelView");
    EXPECT_EQ(abi.pushes[0].members[1].offset, 64U);
    EXPECT_EQ(abi.pushes[0].members[1].size, 64U);
}

TEST(ProgramAbiTest, TheDiffuseSamplerIsTheVariantsFactAndAVariantThatCannotCompileIsRefused)
{
    const auto program = vn::graphics::forwardProgram();

    const ProgramAbi uv = scan(*program, { "VINE_DIFFUSE_MAP", "VINE_TEXCOORD_UV" });
    expectSampler(uv, 0U, 1U, AbiDescriptorKind::Sampler2D, kFragment, "diffuseMap");
    EXPECT_EQ(uv.bindings.size(), 6U);

    const ProgramAbi cube = scan(*program, { "VINE_DIFFUSE_MAP", "VINE_TEXCOORD_CUBE" });
    expectSampler(cube, 0U, 1U, AbiDescriptorKind::SamplerCube, kFragment, "diffuseMap");

    // A texcoord slot that was asked for without its kind: the source says `#error`, and a variant whose own
    // text refuses to compile must not come back as "a program without a diffuse map".
    ProgramFacts facts;
    ASSERT_EQ(buildProgramFacts(*program, facts), FactMiss::None);
    const std::string_view only_map[] = { "VINE_DIFFUSE_MAP" };
    ProgramAbi             refused;
    EXPECT_EQ(scanProgramAbi(facts.shaders.vertex, facts.shaders.fragment, only_map, refused), FactMiss::Malformed);
}

TEST(ProgramAbiTest, TheFlatForwardProgramIsTheSameAbiAndItsDefineComesFromTheText)
{
    const ProgramAbi flat = scan(*vn::graphics::flatForwardProgram());
    const ProgramAbi smooth = scan(*vn::graphics::forwardProgram());
    EXPECT_EQ(flat.bindings.size(), smooth.bindings.size());

    // `VINE_FLAT` is a define the TEXT carries (`#define VINE_FLAT 1`), not one the source imports: the
    // compiler drops a define the source never asks for, so the two must not be conflated.
    const std::vector<std::string> expected = { "VINE_DIFFUSE_MAP", "VINE_TEXCOORD_CUBE", "VINE_TEXCOORD_UV",
                                                "VINE_VERTEX_COLOR" };
    EXPECT_EQ(flat.import_defines, expected);
    EXPECT_EQ(std::find(expected.begin(), expected.end(), "VINE_FLAT"), expected.end());
}

TEST(ProgramAbiTest, ATextsOwnDefineIsInEffectAndAForeignDeclarationsPositionSaysNothing)
{
    constexpr const char* kOwnVertex = R"(#version 450
layout(location = 0) in vec3 position;
void main() { gl_Position = vec4(position, 1.0); }
)";
    constexpr const char* kOwnFragment = R"(#version 450
#define VINE_EXTRA 1
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;
} material;
#ifdef VINE_EXTRA
layout(set = 0, binding = 1, std140) uniform SomeOtherBlock { vec4 value; } extra;
#endif
void main() {}
)";

    ProgramAbi abi;
    ASSERT_EQ(scanProgramAbi(kOwnVertex, kOwnFragment, {}, abi), FactMiss::None);

    // The text defined VINE_EXTRA itself, so the conditional declaration is part of the facts...
    ASSERT_EQ(abi.bindings.size(), 2U);
    // ...and its ROLE is foreign: the block sits where the shipped programs declare the diffuse map, and a
    // position is not a role (only the L1 type name is).
    expectBlock(abi, 0U, 0U, AbiBlockRole::Material, kFragment, sizeof(vn::graphics::VineMaterialBlock));
    const AbiBinding* foreign = at(abi, 0U, 1U);
    ASSERT_NE(foreign, nullptr);
    EXPECT_EQ(foreign->role, AbiBlockRole::Foreign);
    EXPECT_EQ(foreign->type_name, "SomeOtherBlock");
    EXPECT_EQ(foreign->block_size, 16U);
    EXPECT_STREQ(vn::vsg::abiBlockRoleName(foreign->role), "foreign block");
}

TEST(ProgramAbiTest, TheDeferredLightingProgramsDeclareTheirSourcesAndTheShadowedPairAtFive)
{
    const ProgramAbi plain = scanScreen(*vn::graphics::deferredLightProgram());
    ASSERT_EQ(plain.bindings.size(), 4U);
    expectSampler(plain, 0U, 0U, AbiDescriptorKind::Sampler2D, kFragment, "albedo_tex");
    expectSampler(plain, 0U, 1U, AbiDescriptorKind::Sampler2D, kFragment, "normal_tex");
    expectSampler(plain, 0U, 2U, AbiDescriptorKind::Sampler2D, kFragment, "spec_tex");
    expectSampler(plain, 0U, 3U, AbiDescriptorKind::Sampler2D, kFragment, "pos_tex");
    ASSERT_EQ(plain.pushes.size(), 1U);
    EXPECT_EQ(plain.pushes[0].size, 128U);   // the full-screen light block
    EXPECT_EQ(plain.pushes[0].stages, kFragment);
    EXPECT_EQ(at(plain, 0U, 5U), nullptr);   // the unshadowed variant declares no map and no block

    const auto shadowed_program = vn::graphics::shadowedDeferredLightProgram();
    ASSERT_NE(shadowed_program, nullptr) << "the shadow markers must still be in the shipped source";
    const ProgramAbi shadowed = scanScreen(*shadowed_program);
    ASSERT_EQ(shadowed.bindings.size(), 6U);
    expectSampler(shadowed, 0U, 5U, AbiDescriptorKind::Sampler2D, kFragment, "shadow_map");
    expectBlock(shadowed, 0U, 6U, AbiBlockRole::ShadowBlock, kFragment, sizeof(vn::graphics::VineShadowBlock));
}

TEST(ProgramAbiTest, TheScreenProgramsBindTheAttachmentTheirFactoryNamed)
{
    const ProgramAbi first = scanScreen(*vn::graphics::screenCopyProgram(0));
    ASSERT_EQ(first.bindings.size(), 1U);
    expectSampler(first, 0U, 0U, AbiDescriptorKind::Sampler2D, kFragment, "screen_tex");
    EXPECT_TRUE(first.pushes.empty());

    const ProgramAbi third = scanScreen(*vn::graphics::screenCopyProgram(2));
    ASSERT_EQ(third.bindings.size(), 1U);
    expectSampler(third, 0U, 2U, AbiDescriptorKind::Sampler2D, kFragment, "screen_tex");

    // The engine's full-screen vertex stage declares nothing at all: it generates its triangle. It has no
    // fragment stage of its own (the fragment stage is the host's), so its text is scanned as a vertex-only
    // pair - which is exactly how the composed full-screen entry reads it.
    const auto triangle_program = vn::graphics::fullscreenVertexProgram();
    ASSERT_NE(triangle_program, nullptr);
    ASSERT_NE(triangle_program->stage(0), nullptr);
    ProgramAbi triangle;
    ASSERT_EQ(scanProgramAbi(triangle_program->stage(0)->source.as_std_str(), std::string_view{}, {}, triangle),
              FactMiss::None);
    EXPECT_TRUE(triangle.bindings.empty());
    EXPECT_TRUE(triangle.pushes.empty());
}

TEST(ProgramAbiTest, TheGbufferAndSkyboxProgramsDeclareTheSlotsTheirShadingReads)
{
    // The G-buffer geometry stage: the material and the camera matrices, and the map only in the variant
    // that samples one. It declares no lights and (unlike the forward program) no shadow block.
    const ProgramAbi gbuffer = scan(*vn::graphics::gbufferGeometryProgram());
    ASSERT_EQ(gbuffer.bindings.size(), 1U);
    expectBlock(gbuffer, 0U, 0U, AbiBlockRole::Material, kFragment, sizeof(vn::graphics::VineMaterialBlock));
    ASSERT_EQ(gbuffer.pushes.size(), 1U);
    EXPECT_EQ(gbuffer.pushes[0].size, 128U);
    EXPECT_EQ(gbuffer.pushes[0].stages, kVertex);

    const ProgramAbi gbuffer_mapped = scan(*vn::graphics::gbufferGeometryProgram(), { "VINE_DIFFUSE_MAP", "VINE_TEXCOORD_UV" });
    expectSampler(gbuffer_mapped, 0U, 1U, AbiDescriptorKind::Sampler2D, kFragment, "diffuseMap");

    // The sky: its map sits in the same slot the content stages keep for the material's texture, and the
    // sampler KIND follows the texcoord width - including "no kind at all", which is the pair branch.
    const auto skybox = vn::graphics::skyboxProgram();
    const ProgramAbi cube = scan(*skybox, { "VINE_TEXCOORD_CUBE" });
    expectSampler(cube, 0U, 1U, AbiDescriptorKind::SamplerCube, kFragment, "skyMap");
    EXPECT_EQ(cube.bindings.size(), 1U);   // no material, no lights: nothing is lit

    const ProgramAbi pair = scan(*skybox);
    expectSampler(pair, 0U, 1U, AbiDescriptorKind::Sampler2D, kFragment, "skyMap");
    ASSERT_EQ(pair.pushes.size(), 1U);
    EXPECT_EQ(pair.pushes[0].stages, kVertex);
}

TEST(ProgramAbiTest, TheRewriteShapeIsServedByNameNotByPosition)
{
    // The rewrite's own arrangement: the five L1 blocks in set 0 at bindings 0..4 and the pass' samples in
    // set 1. Nothing about it is special to the scan - the roles come from the type names.
    constexpr const char* kOwnVertex = R"(#version 450
layout(location = 0) in vec3 position;
layout(set = 0, binding = 0, std140) uniform VineViewBlock
{
    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame;
} view;
layout(set = 0, binding = 1, std140) uniform VineDrawBlock { mat4 model; vec4 params; } draw;
void main() { gl_Position = view.view_proj * draw.model * vec4(position, 1.0); }
)";
    constexpr const char* kOwnFragment = R"(#version 450
layout(set = 0, binding = 0, std140) uniform VineViewBlock
{
    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame;
} view;
layout(set = 0, binding = 2, std140) uniform VineMaterialBlock
{
    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;
} material;
layout(set = 0, binding = 3, std140) uniform VineLightsBlock
{
    vec4 ambient; vec4 sun_dir[3]; vec4 sun_color[3];
} lights;
layout(set = 0, binding = 4, std140) uniform VineShadowBlock { mat4 viewToLight; vec4 params; } shadow;
layout(set = 1, binding = 0) uniform sampler2D depth_tex;
void main() {}
)";

    ProgramAbi abi;
    ASSERT_EQ(scanProgramAbi(kOwnVertex, kOwnFragment, {}, abi), FactMiss::None);

    ASSERT_EQ(abi.bindings.size(), 6U);
    // Declared by BOTH stages: one binding whose stage flags are the union.
    expectBlock(abi, 0U, 0U, AbiBlockRole::View, kVertex | kFragment, sizeof(vn::graphics::VineViewBlock));
    expectBlock(abi, 0U, 1U, AbiBlockRole::Draw, kVertex, sizeof(vn::graphics::VineDrawBlock));
    expectBlock(abi, 0U, 2U, AbiBlockRole::Material, kFragment, sizeof(vn::graphics::VineMaterialBlock));
    expectBlock(abi, 0U, 3U, AbiBlockRole::Lights, kFragment, sizeof(vn::vsg::VineLightsBlock));
    expectBlock(abi, 0U, 4U, AbiBlockRole::ShadowBlock, kFragment, sizeof(vn::graphics::VineShadowBlock));
    // An unnamed sampler is an input: the serving layer addresses those by binding number, which is how the
    // full-screen ABI names its attachments too.
    expectSampler(abi, 1U, 0U, AbiDescriptorKind::Sampler2D, kFragment, "depth_tex");
}

TEST(ProgramAbiTest, ContradictionsAndUnreadableConditionalsAreRefused)
{
    constexpr const char* kEmpty = "#version 450\nvoid main() {}\n";
    {
        // One (set, binding) cannot be a block in one stage and a sampler in the other.
        constexpr const char* kFragment = R"(#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock { vec4 ambient; } material;
void main() {}
)";
        constexpr const char* kVertex = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D not_a_block;
void main() {}
)";
        ProgramAbi abi;
        EXPECT_EQ(scanProgramAbi(kVertex, kFragment, {}, abi), FactMiss::Malformed);
    }
    {
        // One (set, binding) cannot be two differently sized blocks either.
        constexpr const char* kVertex = R"(#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock { vec4 ambient; } material;
void main() {}
)";
        constexpr const char* kFragment = R"(#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock { vec4 ambient; vec4 diffuse; } material;
void main() {}
)";
        ProgramAbi abi;
        EXPECT_EQ(scanProgramAbi(kVertex, kFragment, {}, abi), FactMiss::Malformed);
    }
    {
        // A binding value this scan cannot read is not a binding to guess at.
        constexpr const char* kFragment = "#version 450\nlayout(set = 0, binding = nope) uniform sampler2D a;\nvoid main() {}\n";
        ProgramAbi abi;
        EXPECT_EQ(scanProgramAbi(kEmpty, kFragment, {}, abi), FactMiss::Malformed);
    }
    {
        // A conditional expression outside the scan's grammar (no `defined`) is reported, not evaluated.
        constexpr const char* kFragment = "#version 450\n#if VINE_SOMETHING\nvoid main() {}\n#endif\n";
        ProgramAbi abi;
        EXPECT_EQ(scanProgramAbi(kEmpty, kFragment, {}, abi), FactMiss::Malformed);
    }
    {
        // An unterminated conditional leaves the text's shape unknown.
        constexpr const char* kFragment = "#version 450\n#ifdef VINE_X\nvoid main() {}\n";
        ProgramAbi abi;
        EXPECT_EQ(scanProgramAbi(kEmpty, kFragment, {}, abi), FactMiss::Malformed);
    }
}

TEST(ProgramAbiTest, ASamplerWithoutABindingIsNotAFactBecauseTheTextStatesNone)
{
    // The compiler assigns a binding to a plain `uniform sampler2D`; nothing in the text says which one, so
    // it is not a declared binding - the serving layer must not invent a slot for it either.
    constexpr const char* kEmpty    = "#version 450\nvoid main() {}\n";
    constexpr const char* kFragment = "#version 450\nuniform sampler2D unnamed;\nvoid main() {}\n";
    ProgramAbi             abi;
    ASSERT_EQ(scanProgramAbi(kEmpty, kFragment, {}, abi), FactMiss::None);
    EXPECT_TRUE(abi.bindings.empty());
}

TEST(ProgramAbiTest, ADriftedBlockSizeIsVisibleAsAFact)
{
    // The same block the L1 header pins at 64 bytes, in a text that grew a member: the scan reports what the
    // TEXT says (80), so the layer that binds the L1 bytes can see the mismatch instead of assuming it away.
    constexpr const char* kEmpty = "#version 450\nvoid main() {}\n";
    constexpr const char* kFragment = R"(#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 ambient; vec4 diffuse; vec4 specular; float shininess; vec4 extra;
} material;
void main() {}
)";
    ProgramAbi abi;
    ASSERT_EQ(scanProgramAbi(kEmpty, kFragment, {}, abi), FactMiss::None);
    const AbiBinding* material = at(abi, 0U, 0U);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->block_size, 80U);
    EXPECT_NE(material->block_size, sizeof(vn::graphics::VineMaterialBlock));
}
