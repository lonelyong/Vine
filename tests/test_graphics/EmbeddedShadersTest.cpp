/**
 * @brief The shaders are embedded into the binary as whole GLSL sources (see cmake/VineShaders.cmake).
 *
 * The SDK's programs are no longer C++ string literals: each stage is a real .vert/.frag file
 * under src/viz/graphics/shaders/, turned into string constants at build time. That
 * indirection is invisible to the renderer, so what these tests pin is that the
 * generated table is intact and is what the default programs actually use:
 *
 *  * every entry is a complete GLSL source (prologue, a main, closing brace) and not a
 *    truncated or line-ending-mangled copy;
 *  * the bookkeeping (name / hash / byte count) agrees with the text, so a corrupt
 *    embedding cannot pass unnoticed;
 *  * the default forward / G-buffer / deferred-light programs compile from exactly
 *    these constants, which is what makes editing the .glsl file the only way to
 *    change them.
 *
 * scripts/vn_shader_check.sh adds the two things a unit test cannot do: it compiles
 * every shader with glslangValidator and verifies the embedded hashes against the
 * files on disk.
 */

#include <gtest/gtest.h>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ChainPresets.hpp>
#include <vine/graphics/EmbeddedShaders.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vn::graphics;
using namespace vn::graphics::shaders;

namespace
{

/**
 * @brief Checks that @p text is one complete GLSL source.
 *
 * @param name Entry name, used in the failure messages.
 * @param text Source text to inspect.
 */
void expectWholeGlslSource(std::string_view name, std::u8string_view text)
{
    ASSERT_FALSE(text.empty()) << name;
    ASSERT_TRUE(text.starts_with(u8"#version 450\n")) << name;
    ASSERT_NE(text.find(u8"void main("), std::u8string_view::npos) << name;
    ASSERT_TRUE(text.ends_with(u8"}\n")) << name;
}

/**
 * @brief Checks that @p text is 16 lowercase hexadecimal digits.
 *
 * @param hash Hash field to inspect.
 * @return true when the field could be an abbreviated SHA-256.
 */
bool isAbbreviatedSha256(std::string_view hash)
{
    return hash.size() == 16 &&
           std::all_of(hash.begin(), hash.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

/**
 * @brief Finds the embedded entry generated from @p file_name.
 *
 * @param file_name Source file name, e.g. "builtin_gbuffer.vert".
 * @return Pointer to the entry, or null when it was not embedded.
 */
const Entry* findEntry(std::string_view file_name)
{
    const auto it = std::find_if(std::begin(kAll), std::end(kAll),
                                 [file_name](const Entry& entry) { return entry.name == file_name; });
    return it == std::end(kAll) ? nullptr : &*it;
}

TEST(EmbeddedShadersTest, EveryShaderIsAWholeGlslSource)
{
    ASSERT_GT(kCount, 0u);
    for (const Entry& entry : kAll) {
        ASSERT_FALSE(entry.name.empty());
        expectWholeGlslSource(std::string(entry.name), entry.source);
    }
}

TEST(EmbeddedShadersTest, BookkeepingAgreesWithTheText)
{
    std::vector<std::string> names;
    for (const Entry& entry : kAll) {
        ASSERT_EQ(entry.bytes, entry.source.size()) << entry.name;
        ASSERT_TRUE(isAbbreviatedSha256(entry.hash)) << entry.name << ": " << entry.hash;
        names.emplace_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    ASSERT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

/** @brief Reinterprets UTF-8 shader text as a searchable byte string (defined below). */
std::string asByteString(std::u8string_view text);

/**
 * @brief The two lines a shipped source marks the shadow ABI's insertion points with.
 *
 * @return The markers, as byte strings (the sources are char8_t).
 */
std::vector<std::string> shadow_markers()
{
    return { "// VN_SHADOW_BINDINGS\n", "// VN_SHADOW_TERM\n" };
}

/**
 * @brief Asserts @p text is @p source with its marker LINES replaced by something.
 *
 * Every segment of the shipped file between the markers must survive verbatim and in order, which is
 * what "the program is derived from the file" means when the inserted text is not this test's
 * business. A segment that moved, was reworded, or was dropped fails here — the drift a duplicated
 * expectation would not catch.
 *
 * @param text    Program text to check.
 * @param source  Shipped source it must be derived from.
 * @param markers Marker lines the source carries (each standing alone on its line).
 */
void expectDerivedFromSource(const std::string& text, const std::string& source,
                             const std::vector<std::string>& markers)
{
    std::size_t              cursor = 0;   // how far into @p text the last segment ended
    std::vector<std::size_t> cuts;
    for (const std::string& marker : markers) {
        const std::size_t at = source.find(marker);
        ASSERT_NE(at, std::string::npos) << "the shipped source carries each insertion marker";
        // The cut is the START OF THE MARKER'S LINE, not the marker itself: the insertion replaces
        // the whole line, so its indentation goes with it and a segment that kept those spaces would
        // never be found (the marker stands alone on its line precisely so this is well defined).
        cuts.push_back(source.rfind('\n', at) + 1u);
    }
    std::sort(cuts.begin(), cuts.end());
    std::size_t start = 0;
    for (const std::size_t cut : cuts) {
        const std::string segment = source.substr(start, cut - start);
        const std::size_t found   = text.find(segment, cursor);
        EXPECT_NE(found, std::string::npos) << "the program text must carry this segment of the file, which ends at\n  "
                                           << segment.substr(segment.size() > 80u ? segment.size() - 80u : 0u);
        if (found == std::string::npos) {
            return;
        }
        cursor = found + segment.size();
        // Skip the marker line itself: it is what the insertion replaced.
        start = source.find('\n', cut) + 1u;
    }
    const std::string tail = source.substr(start);
    EXPECT_NE(text.find(tail, cursor), std::string::npos)
        << "the program text must carry the file's tail, which starts at\n  " << tail.substr(0, 80u);
}

TEST(EmbeddedShadersTest, TheDeferredProgramsUseTheEmbeddedSources)
{
    const auto gbuffer = defaultGbufferGeometryProgram();
    ASSERT_NE(gbuffer, nullptr);
    ASSERT_EQ(gbuffer->stageCount(), 2u);
    const ShaderStage* geometry_vs = gbuffer->stage(0);
    const ShaderStage* geometry_fs = gbuffer->stage(1);
    ASSERT_NE(geometry_vs, nullptr);
    ASSERT_NE(geometry_fs, nullptr);
    EXPECT_EQ(geometry_vs->type, ShaderStageType::Vertex);
    EXPECT_EQ(geometry_fs->type, ShaderStageType::Fragment);
    EXPECT_EQ(geometry_vs->source, vn::String(kBuiltinGbufferVert));
    EXPECT_EQ(geometry_fs->source, vn::String(kBuiltinGbufferFrag));

    const auto light = defaultDeferredLightProgram();
    ASSERT_NE(light, nullptr);
    ASSERT_EQ(light->stageCount(), 1u);
    const ShaderStage* light_fs = light->stage(0);
    ASSERT_NE(light_fs, nullptr);
    EXPECT_EQ(light_fs->type, ShaderStageType::Fragment);
    // The lighting program is the embedded source with its two insertion markers taken out (the
    // shadowed variant is the same source with text put in their place): the FILE is what ships, and
    // a program that is not derived from it is a second copy that drifts.
    const std::string shipped  = asByteString(kBuiltinDeferredLightingFrag);
    const std::string markers  = "// VN_SHADOW_BINDINGS\n";
    const std::string markers2 = "// VN_SHADOW_TERM\n";
    std::string       expected = shipped;
    for (const std::string& marker : { markers, markers2 }) {
        const std::size_t at = expected.find(marker);
        ASSERT_NE(at, std::string::npos) << "the shipped source carries the insertion markers";
        expected.erase(at, marker.size());
    }
    EXPECT_EQ(light_fs->source.as_std_str(), expected);
}

TEST(EmbeddedShadersTest, TheBuiltinForwardProgramsUseTheEmbeddedSources)
{
    // Scene shading is an SDK program like the deferred ones: its stages must be exactly the embedded
    // forward GLSL, so the program the engine hands out and the file on disk cannot drift apart.
    const auto forward = forwardProgram();
    ASSERT_NE(forward, nullptr);
    ASSERT_EQ(forward->stageCount(), 2u);
    const ShaderStage* forward_vs = forward->stage(0);
    const ShaderStage* forward_fs = forward->stage(1);
    ASSERT_NE(forward_vs, nullptr);
    ASSERT_NE(forward_fs, nullptr);
    EXPECT_EQ(forward_vs->type, ShaderStageType::Vertex);
    EXPECT_EQ(forward_fs->type, ShaderStageType::Fragment);
    EXPECT_EQ(forward_vs->source, vn::String(kBuiltinForwardVert));
    // The fragment stage is that file with the shadow ABI inserted at its markers (the content set is
    // shared per (target, depth mode), so the ABI is declared UNCONDITIONALLY and `shadow.params.x`
    // is the runtime switch — see BuiltinShaders::forwardProgram). What this checks is that every
    // SEGMENT of the shipped file survives verbatim and in order: the inserted text has its own test
    // (TheForwardProgramDeclaresTheShadowAbiWhereTheContentSetBindsIt), and copying it here would be a
    // second copy to keep in step — the drift this test exists to catch.
    expectDerivedFromSource(forward_fs->source.as_std_str(), asByteString(kBuiltinForwardFrag), shadow_markers());

    // The flat program is the SAME stages with one define injected into the fragment source — which is
    // what makes it a different program (a different text), not a mode of another one. The vertex stage
    // is the same object's text, so the two can only differ where the define is read.
    const auto flat = flatForwardProgram();
    ASSERT_NE(flat, nullptr);
    ASSERT_EQ(flat->stageCount(), 2u);
    const ShaderStage* flat_vs = flat->stage(0);
    const ShaderStage* flat_fs = flat->stage(1);
    ASSERT_NE(flat_vs, nullptr);
    ASSERT_NE(flat_fs, nullptr);
    EXPECT_EQ(flat_vs->source, vn::String(kBuiltinForwardVert));
    EXPECT_NE(flat_fs->source, vn::String(kBuiltinForwardFrag));
    EXPECT_NE(flat_fs->source.as_std_str().find("#define VN_FLAT 1"), std::string::npos);
    // ...and it is that text with nothing else changed: the define goes in after the version directive,
    // so dropping that one line has to give back the forward program EXACTLY. Two programs that share a
    // lighting must share its text too — otherwise the flat preset is a second copy that drifts.
    std::string       flat_without_define = flat_fs->source.as_std_str();
    const std::string define_line         = "#define VN_FLAT 1\n";
    const std::size_t define_at           = flat_without_define.find(define_line);
    ASSERT_NE(define_at, std::string::npos);
    flat_without_define.erase(define_at, define_line.size());
    EXPECT_EQ(flat_without_define, forward_fs->source.as_std_str())
        << "the flat program is the forward text plus one define, not a second copy of the shading";
}

/**
 * @brief The "layout(location = N) in" text an attribute declaration starts with.
 *
 * @param location Shader location.
 * @return The declaration prefix.
 */
std::string attributeLayout(std::uint32_t location)
{
    return "layout(location = " + std::to_string(location) + ") in";
}

/**
 * @brief Reinterprets UTF-8 shader text as a searchable byte string.
 *
 * @param text Shader source.
 * @return The same bytes as a std::string.
 */
std::string asByteString(std::u8string_view text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

TEST(EmbeddedShadersTest, TheAbiLocationsMatchTheShaderText)
{
    // The attribute location table is the engine's ABI (ShaderAbi.hpp); the shader
    // text must declare each role where the table says. A location changed in one
    // place only is exactly the drift this pins.
    EXPECT_EQ(attributeLocation(VertexAttribute::Position), 0u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Normal), 1u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Color), 2u);
    EXPECT_EQ(attributeLocation(VertexAttribute::TexCoord0), 8u);

    const std::string forward_vs = asByteString(kBuiltinForwardVert);
    for (const auto role : { VertexAttribute::Position, VertexAttribute::Normal, VertexAttribute::Color,
                             VertexAttribute::TexCoord0 }) {
        EXPECT_NE(forward_vs.find(attributeLayout(attributeLocation(role))), std::string::npos)
            << "vertex attribute at location " << attributeLocation(role);
    }
}

TEST(EmbeddedShadersTest, TheLightBlockDeclaresOneSlotPerLightTheCapacityAllows)
{
    // kMaxLights is the ONE statement of how many lights a drawing call can have lit (ShaderAbi.hpp); the light
    // block's GLSL arrays are its other end. The block struct's sizeof static_assert pins the C++ side, and this
    // pins the GLSL side, so raising the capacity in one place only is a failing test rather than a block whose
    // arrays the shader reads past.
    const std::string slots = "[" + std::to_string(kMaxLights) + "]";
    for (const std::string_view name : { "builtin_forward.frag", "builtin_deferred_lighting.frag" })
    {
        std::string source;
        for (const Entry& entry : kAll)
        {
            if (entry.name == name)
            {
                source = asByteString(entry.source);
                break;
            }
        }
        ASSERT_FALSE(source.empty()) << name << " is embedded";
        for (const std::string_view field : { "position_kind", "direction_range", "color_intensity", "cone" })
        {
            const std::string declaration = "vec4 " + std::string(field) + slots;
            EXPECT_NE(source.find(declaration), std::string::npos)
                << name << " declares " << declaration << " (kMaxLights is " << kMaxLights << ")";
        }
    }
}

TEST(EmbeddedShadersTest, TheContentVertexStagesSizeTheirPointsFromTheDrawBlock)
{
    // A POINT_LIST pipeline needs a vertex stage that STATICALLY writes gl_PointSize, and the engine's size is the
    // draw block's `params.y` (a StateNode's setPointSize or the pass' override, already resolved). A stage that
    // wrote a constant instead would silently ignore both, which is the failure this pins.
    for (const std::u8string_view source : { kBuiltinForwardVert, kBuiltinGbufferVert })
    {
        const std::string text = asByteString(source);
        EXPECT_NE(text.find("#define VN_PARAM_DRAW"), std::string::npos)
            << "the vertex stage must declare the draw block it reads the size from";
        EXPECT_NE(text.find("#include \"vine_params.glsl\""), std::string::npos)
            << "and take the declaration from the ONE fragment, never a copy";
        EXPECT_NE(text.find("gl_PointSize = vn_draw.params.y"), std::string::npos)
            << "the size is the per-draw value, not a literal";
    }
}

TEST(EmbeddedShadersTest, TheMaterialSlotBitsAgreeWithTheBlockAbi)
{
    // ShaderAbi.hpp is the C++ half of the material block (kMaterialFlagBaseColor .. kMaterialFlagEmissive,
    // kMaterialAlphaModeShift) and vine_params.glsl is its GLSL half. GLSL cannot ask C++, so the numbers are
    // written twice - and a bit moved in one place only makes every textured material read its neighbour's
    // slot (or none), with no layer reporting it. This pins the two lists against each other.
    const auto declared = [](std::string_view name, std::uint32_t value) {
        std::uint32_t bit = 0;
        while ((value >> bit) != 1U) {
            ++bit;
        }
        // The declarations are aligned with spaces, so the NAME is searched first and the initialiser after it.
        return std::pair<std::string, std::string>{ std::string(name),
                                                    "= 1u << " + std::to_string(bit) + ";" };
    };
    const std::string shading_stages = asByteString(kBuiltinForwardFrag) + asByteString(kBuiltinGbufferFrag);
    for (const auto& [name, value] : { std::pair<std::string_view, std::uint32_t>{
                                          "VN_MATERIAL_FLAG_BASE_COLOR", kMaterialFlagBaseColor },
                                      { "VN_MATERIAL_FLAG_METALLIC_ROUGHNESS", kMaterialFlagMetallicRoughness },
                                      { "VN_MATERIAL_FLAG_NORMAL", kMaterialFlagNormal },
                                      { "VN_MATERIAL_FLAG_OCCLUSION", kMaterialFlagOcclusion },
                                      { "VN_MATERIAL_FLAG_EMISSIVE", kMaterialFlagEmissive } }) {
        const auto [declaration, initialiser] = declared(name, value);
        const std::size_t at                  = shading_stages.find(declaration);
        ASSERT_NE(at, std::string::npos) << declaration << " must be declared in vine_params.glsl";
        EXPECT_NE(shading_stages.find(initialiser, at), std::string::npos)
            << declaration << " must take " << initialiser << " (ShaderAbi.hpp's value is " << value << ")";
    }
    const std::size_t shift_at = shading_stages.find("VN_MATERIAL_ALPHA_MODE_SHIFT");
    ASSERT_NE(shift_at, std::string::npos) << "the alpha mode's shift is part of the block's GLSL";
    EXPECT_NE(shading_stages.find("= " + std::to_string(kMaterialAlphaModeShift) + "u;", shift_at),
              std::string::npos)
        << "the alpha mode's shift is the block's, not a literal a shader invents";
}

TEST(EmbeddedShadersTest, TheSharedFragmentsAreInlinedIntoTheStagesThatUseThem)
{
    // The shading is split across fragments that both paths include (vine_brdf.glsl is the BRDF's one home,
    // vine_material.glsl the texture set's, vine_gbuffer.glsl the attachment packing's). What ships is the
    // EXPANDED text (cmake/VineEmbedShaders.cmake), so a stage's source carries the function - and a stage that
    // lost an include still compiles, it just shades with whatever it happens to have. These assertions are the
    // cheap half of that: the functions each stage is supposed to be made of are IN its text.
    const auto carries = [](std::u8string_view source, std::string_view needle, std::string_view stage) {
        EXPECT_NE(asByteString(source).find(needle), std::string::npos)
            << stage << " must carry " << needle << " (its fragment was not inlined)";
    };
    carries(kBuiltinForwardFrag, "vec3 vnEvaluateBRDF(", "builtin_forward.frag");
    carries(kBuiltinForwardFrag, "VineSurface vnSampleMaterial(", "builtin_forward.frag");
    carries(kBuiltinGbufferFrag, "vec2 vnOctahedralEncode(", "builtin_gbuffer.frag");
    carries(kBuiltinGbufferFrag, "VineSurface vnSampleMaterial(", "builtin_gbuffer.frag");
    carries(kBuiltinDeferredLightingFrag, "vec3 vnEvaluateBRDF(", "builtin_deferred_lighting.frag");
    carries(kBuiltinDeferredLightingFrag, "vec3 vnOctahedralDecode(", "builtin_deferred_lighting.frag");
    // The retired bridge: neither helper may come back, because a shader that shades Phong terms again is a
    // second shading model beside the BRDF, and nothing would compare the two.
    for (const std::u8string_view source : { kBuiltinForwardFrag, kBuiltinGbufferFrag, kBuiltinDeferredLightingFrag }) {
        EXPECT_EQ(asByteString(source).find("vineMaterialShininess"), std::string::npos)
            << "the M1 bridge's exponent helper is gone (graphics-material.md 9.2, M3)";
        EXPECT_EQ(asByteString(source).find("vineMaterialF0"), std::string::npos);
    }
}

TEST(EmbeddedShadersTest, TheGbufferWriterAndTheLightingPassShareOnePacking)
{
    // The two halves of the G-buffer contract: the writer encodes and the lighting pass decodes with the SAME
    // octahedral pair (vine_gbuffer.glsl). If one of them were inlined from a stale copy - or hand-rolled - the
    // deferred picture would rotate every normal and no layer would report it.
    const std::string writer   = asByteString(kBuiltinGbufferFrag);
    const std::string lighting = asByteString(kBuiltinDeferredLightingFrag);
    EXPECT_NE(writer.find("vnOctahedralEncode"), std::string::npos);
    EXPECT_NE(lighting.find("vnOctahedralDecode"), std::string::npos);
    // The factors ride the normal attachment (the packing's own rule): b = roughness, a = metallic on the write
    // side, and the same two channels on the read side.
    EXPECT_NE(writer.find("vnOctahedralEncode(vn_surface.normal), vn_surface.roughness, vn_surface.metallic"),
              std::string::npos)
        << "the writer stores the normal and both factors in one row";
    EXPECT_NE(lighting.find("vnOctahedralDecode(vn_n4.rg)"), std::string::npos);
    EXPECT_NE(lighting.find("clamp(vn_n4.b, 0.0, 1.0)"), std::string::npos) << "b is the roughness";
    EXPECT_NE(lighting.find("clamp(vn_n4.a, 0.0, 1.0)"), std::string::npos) << "a is the metallic";
}

TEST(EmbeddedShadersTest, TheShadowedLightingProgramIsBuiltFromMarkersTheSourceCarries)
{
    // The shadowed variant of the lighting program is the shipped source with two insertions, and
    // its bindings are part of the shadow ABI (see BuiltinShaders::deferredLightProgram): the map
    // at binding 7 and its block at 8, right after the canonical G-buffer's four colours, its depth slot and
    // the lights at 6. A source edit that moves or renames a marker must fail HERE — the alternative is
    // a program that builds without complaint and whose shadow terms are whatever used to follow.
    const std::string source = asByteString(kBuiltinDeferredLightingFrag);
    EXPECT_NE(source.find("// VN_SHADOW_BINDINGS"), std::string::npos) << "the declarations' marker";
    EXPECT_NE(source.find("// VN_SHADOW_TERM"), std::string::npos) << "the term's marker";

    const auto plain = deferredLightProgram();
    ASSERT_NE(plain, nullptr);
    const std::string plain_text = plain->stage(0)->source.as_std_str();
    EXPECT_EQ(plain_text.find("VN_SHADOW_"), std::string::npos)
        << "the unshadowed program must ask the pass for no shadow binding at all";
    EXPECT_EQ(plain_text.find("vn_shadow_map"), std::string::npos);

    const auto shadowed = shadowedDeferredLightProgram();
    ASSERT_NE(shadowed, nullptr);
    const std::string shadowed_text = shadowed->stage(0)->source.as_std_str();
    EXPECT_NE(shadowed_text.find("layout(binding = 7) uniform sampler2D vn_shadow_map;"), std::string::npos);
    EXPECT_NE(shadowed_text.find("layout(binding = 8, std140) uniform VineShadowBlock"), std::string::npos)
        << "the block's GLSL type name is the L1 name (ShaderAbi.hpp)";
    EXPECT_NE(shadowed_text.find("vn_shadow.viewToLight"), std::string::npos);
    EXPECT_EQ(shadowed_text.find("VN_SHADOW_BINDINGS"), std::string::npos)
        << "the markers are scaffolding: the program text must not carry them";
    EXPECT_EQ(shadowed_text.find("VN_SHADOW_TERM"), std::string::npos);
}

TEST(EmbeddedShadersTest, TheShadowTermNamesTheLightItsMapBelongsTo)
{
    // The term is inserted INSIDE the loop over the block's typed lights, so the ownership guard is the only
    // thing keeping one light's map from scaling every other light's term: `shadow.params.w` is the packed
    // slot of the light the map belongs to (ShaderAbi.hpp), and the backend writes it from the map's own
    // statement (RenderTarget::setShadowOf -> CompiledPass::shadow -> packedSlotOf). A term without it shades
    // a shadow onto lights that cast nothing - measured: a face the sun never reaches went to ambient-only in
    // the shape of the sun's shadow, and the same file's single-light fixtures could not see the difference at
    // all.
    for (const auto& program : { shadowedDeferredLightProgram(), forwardProgram() }) {
        ASSERT_NE(program, nullptr);
        ASSERT_GT(program->stageCount(), 0u);
        const std::string text = program->stage(program->stageCount() - 1u)->source.as_std_str();
        EXPECT_NE(text.find("vn_i == int(vn_shadow.params.w)"), std::string::npos)
            << "the term must guard on the light's own slot, not only on the ABI's on/off switch";
    }
}

TEST(EmbeddedShadersTest, TheForwardProgramDeclaresTheShadowAbiWhereTheContentSetBindsIt)
{
    // The forward program carries the shadow ABI UNCONDITIONALLY (unlike the deferred lighting
    // program's two variants) and at the CONTENT path's bindings: the content set is shared per
    // (target, depth mode) and a session picks the content program once, so a shadowed variant would
    // double that cache for every target and a host's own program would still have no shadowed twin to
    // pick. One text, both paths, switched at runtime by `shadow.params.x` — which is what the ABI's
    // params block is for (ShaderAbi.hpp).
    //
    // The bindings are pinned HERE, in the shipped text's own terms, because they are an ABI two
    // layers have to agree on: the fragment program's layout qualifiers must name what the content set
    // declares (set 0, the map at 7, the block at 8, after the material at 0, the five texture slots at
    // 1-5 and the lights at 6). A number changed in one place only is a shader reading somebody else's
    // binding.
    const auto forward = forwardProgram();
    ASSERT_NE(forward, nullptr);
    ASSERT_EQ(forward->stageCount(), 2u);
    const std::string text = forward->stage(1)->source.as_std_str();
    EXPECT_NE(text.find("layout(set = 0, binding = 7) uniform sampler2D vn_shadow_map;"), std::string::npos);
    EXPECT_NE(text.find("layout(set = 0, binding = 8, std140) uniform VineShadowBlock"), std::string::npos)
        << "the block's GLSL type name is the L1 name (ShaderAbi.hpp)";
    EXPECT_NE(text.find("vn_shadow.viewToLight"), std::string::npos);
    // Which switch the term reads, so the two paths cannot drift into different ABIs.
    EXPECT_NE(text.find("vn_shadow.params.x"), std::string::npos);
    EXPECT_EQ(text.find("VN_SHADOW_BINDINGS"), std::string::npos)
        << "the markers are scaffolding: the program text must not carry them";
    EXPECT_EQ(text.find("VN_SHADOW_TERM"), std::string::npos);
    // The term reads the fragment's view position by ONE name in both programs (the forward path has
    // it as a varying, the deferred one reads it out of the G-buffer), which is what makes the term
    // text shareable — so the alias has to be there.
    EXPECT_NE(text.find("vec3 vn_pos = vn_view_pos;"), std::string::npos);
}

TEST(EmbeddedShadersTest, TheSkyboxProgramUsesTheEmbeddedSources)
{
    // The sky program is an SDK program like the forward and deferred ones: its stages must be exactly
    // the embedded skybox GLSL, so the program the engine hands out and the file on disk cannot drift
    // apart. Editing the .vert/.frag is therefore the only way to change a sky.
    const auto skybox = skyboxProgram();
    ASSERT_NE(skybox, nullptr);
    ASSERT_EQ(skybox->stageCount(), 2u);
    const ShaderStage* skybox_vs = skybox->stage(0);
    const ShaderStage* skybox_fs = skybox->stage(1);
    ASSERT_NE(skybox_vs, nullptr);
    ASSERT_NE(skybox_fs, nullptr);
    EXPECT_EQ(skybox_vs->type, ShaderStageType::Vertex);
    EXPECT_EQ(skybox_fs->type, ShaderStageType::Fragment);
    EXPECT_EQ(skybox_vs->source, vn::String(kBuiltinSkyboxVert));
    EXPECT_EQ(skybox_fs->source, vn::String(kBuiltinSkyboxFrag));
    // Its sampler kind follows the texcoord width, which is the contract the backend's kind check keys
    // on (see the header): both kinds and both branches have to be there, in the stages that declare
    // them. The names must be in the PRAGMA too - a define a source does not ask for is dropped in
    // silence - so a sky that never named the UV kind could not be built as a UV pair at all.
    EXPECT_NE(skybox_vs->source.as_std_str().find("VN_TEXCOORD_CUBE"), std::string::npos)
        << "the vertex stage must name the define it branches on (a program that does not ask for it is "
           "never given it)";
    EXPECT_NE(skybox_vs->source.as_std_str().find("VN_TEXCOORD_UV"), std::string::npos);
    EXPECT_NE(skybox_fs->source.as_std_str().find("VN_TEXCOORD_UV"), std::string::npos);
    EXPECT_NE(skybox_fs->source.as_std_str().find("samplerCube vn_skyMap"), std::string::npos);
    EXPECT_NE(skybox_fs->source.as_std_str().find("sampler2D vn_skyMap"), std::string::npos);
}

TEST(EmbeddedShadersTest, AScreenCopyCanShowOneChannelOfItsAttachment)
{
    // A copy of an attachment whose content rides in its ALPHA (the G-buffer's emission + occlusion row -
    // emission is HDR and usually zero) is the same shipped text with its OUTPUT expression changed, so the
    // marker it rewrites has to be in the source: a rewrite that silently missed would leave a copy of the rgb,
    // which is the black rectangle this variant exists to replace.
    const std::string shipped = asByteString(kBuiltinScreenCopyFrag);
    EXPECT_NE(shipped.find("vn_out_color = texture(vn_screen_tex, vn_uv);"), std::string::npos)
        << "the factory rewrites this line; a source edit must not remove it quietly";

    const auto plain = screenCopyProgram(2);
    ASSERT_NE(plain, nullptr);
    ASSERT_GT(plain->stageCount(), 0u);
    const std::string plain_text = plain->stage(0)->source.as_std_str();
    EXPECT_NE(plain_text.find("layout(binding = 2) uniform sampler2D vn_screen_tex;"), std::string::npos)
        << "the attachment is the binding";
    EXPECT_NE(plain_text.find("vn_out_color = texture(vn_screen_tex, vn_uv);"), std::string::npos)
        << "the plain variant keeps the shipped output";

    const auto alpha = screenCopyProgram(2, ScreenCopyChannels::Alpha);
    ASSERT_NE(alpha, nullptr);
    ASSERT_GT(alpha->stageCount(), 0u);
    const std::string alpha_text = alpha->stage(0)->source.as_std_str();
    EXPECT_NE(alpha_text.find("vn_out_color = vec4(vec3(texture(vn_screen_tex, vn_uv).a), 1.0);"), std::string::npos)
        << "the alpha variant copies that channel as grey";
    EXPECT_EQ(alpha_text.find("vn_out_color = texture(vn_screen_tex, vn_uv);"), std::string::npos)
        << "and nothing of the plain output is left";
    EXPECT_NE(plain->name(), alpha->name()) << "two programs, two names (a pool keys pipelines by identity)";
    EXPECT_EQ(screenCopyProgram()->name(), u8"builtin_screen_copy") << "the no-argument form is the plain copy";
    EXPECT_EQ(screenCopyProgram(0, ScreenCopyChannels::Alpha)->name(), u8"builtin_screen_copy_alpha");
}

TEST(EmbeddedShadersTest, TheNamedConstantsAreInTheTable)
{
    ASSERT_NE(findEntry("builtin_forward.vert"), nullptr);
    ASSERT_NE(findEntry("builtin_forward.frag"), nullptr);
    ASSERT_NE(findEntry("builtin_gbuffer.vert"), nullptr);
    ASSERT_NE(findEntry("builtin_gbuffer.frag"), nullptr);
    ASSERT_NE(findEntry("builtin_deferred_lighting.frag"), nullptr);
    ASSERT_NE(findEntry("builtin_skybox.vert"), nullptr);
    ASSERT_NE(findEntry("builtin_skybox.frag"), nullptr);
    EXPECT_EQ(findEntry("builtin_forward.vert")->source, kBuiltinForwardVert);
    EXPECT_EQ(findEntry("builtin_forward.frag")->source, kBuiltinForwardFrag);
    EXPECT_EQ(findEntry("builtin_gbuffer.vert")->source, kBuiltinGbufferVert);
    EXPECT_EQ(findEntry("builtin_gbuffer.frag")->source, kBuiltinGbufferFrag);
    EXPECT_EQ(findEntry("builtin_deferred_lighting.frag")->source, kBuiltinDeferredLightingFrag);
    EXPECT_EQ(findEntry("builtin_skybox.vert")->source, kBuiltinSkyboxVert);
    EXPECT_EQ(findEntry("builtin_skybox.frag")->source, kBuiltinSkyboxFrag);
}

/**
 * @brief Checks @p name against the naming rule the inventory states (cmake/VineShaders.cmake).
 *
 * The rule is `builtin_<role>.<stage>`: `builtin_` says the text is the engine's own (a host's
 * shader is the one thing the directory never holds), the role is the renderer's word for what the
 * stage does, and the stage suffix is what the offline validator reads the stage from. The rule is
 * a GATE and not a comment because the names it rejects were all real: `vn_forward.*` carried a
 * product prefix, `std_forward.*` read as the C++ standard library's `std::forward`, and a role
 * word that repeats the stage (`builtin_gbuffer_vert.vert`) says nothing twice.
 *
 * What a name CHECK cannot see is whether the role is the right word — `builtin_gbuffer_geometry.frag`
 * is legal here and still says its target twice; that half stays a review rule (the role is the pass'
 * word, see the inventory's comment).
 *
 * @param name Embedded entry name (the source file's name).
 * @return Empty when the name obeys the rule, else the reason it does not.
 */
std::string nameRuleViolation(std::string_view name)
{
    constexpr std::string_view stages[] = { ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese" };
    const std::size_t          dot      = name.rfind('.');
    if (dot == std::string_view::npos) {
        return "no stage suffix";
    }
    const std::string_view stage = name.substr(dot);
    if (std::find(std::begin(stages), std::end(stages), stage) == std::end(stages)) {
        return "stage suffix is not one of .vert/.frag/.comp/.geom/.tesc/.tese";
    }

    const std::string_view stem = name.substr(0u, dot);
    if (!stem.starts_with("builtin_")) {
        return "does not start with builtin_ (the prefix is what tells the engine's text from a host's)";
    }
    const std::string_view role = stem.substr(8u);
    if (role.empty()) {
        return "no role between the prefix and the stage";
    }
    if (role.starts_with("std_") || role == "std") {
        return "std_ is the C++ standard library's namespace, not a renderer's word";
    }
    if (role.starts_with("vn_") || role.starts_with("vsg_")) {
        return "a product prefix belongs to the module namespace, not to a shading role";
    }
    if (role.front() == '_' || role.back() == '_' || role.find("__") != std::string_view::npos) {
        return "role has an empty word (leading, trailing or doubled underscore)";
    }
    for (const char c : role) {
        const bool word_char = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!word_char) {
            return std::string("role is not lowercase snake_case: '") + c + "'";
        }
    }
    // A role that spells its own stage out again (`..._vert.vert`) is the one duplication a machine
    // can see, so it is the one the gate takes.
    std::size_t word_start = 0u;
    while (word_start <= role.size()) {
        const std::size_t word_end = std::min(role.find('_', word_start), role.size());
        const std::string_view word = role.substr(word_start, word_end - word_start);
        if (word == stage.substr(1u)) {
            return "role repeats the stage suffix ('" + std::string(word) + "')";
        }
        word_start = word_end + 1u;
    }
    return {};
}

TEST(EmbeddedShadersTest, EveryShaderNameFollowsTheInventorysRule)
{
    for (const Entry& entry : kAll) {
        EXPECT_TRUE(nameRuleViolation(entry.name).empty())
            << entry.name << ": " << nameRuleViolation(entry.name);
    }
    // The gate has to be able to fail, and on names this repository has actually used or flirted with.
    EXPECT_FALSE(nameRuleViolation("std_forward.vert").empty());
    EXPECT_FALSE(nameRuleViolation("vn_forward.frag").empty());
    EXPECT_FALSE(nameRuleViolation("vsg_phong.frag").empty());
    EXPECT_FALSE(nameRuleViolation("builtin_forward.hlsl").empty());
    EXPECT_FALSE(nameRuleViolation("builtin_Forward.vert").empty());
    EXPECT_FALSE(nameRuleViolation("builtin_forward_vert.vert").empty());
    EXPECT_FALSE(nameRuleViolation("forward.vert").empty());
    EXPECT_FALSE(nameRuleViolation("builtin_forward").empty());
    EXPECT_FALSE(nameRuleViolation("builtin_forward__temp.vert").empty());
    EXPECT_TRUE(nameRuleViolation("builtin_forward.vert").empty());
    EXPECT_TRUE(nameRuleViolation("builtin_deferred_lighting.frag").empty());
    EXPECT_TRUE(nameRuleViolation("builtin_screen_copy.frag").empty());
}

}  // namespace
