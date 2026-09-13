#include <vine/graphics/BuiltinShaders.hpp>

#include <string_view>

#include <vine/graphics/EmbeddedShaders.hpp>

V_GRAPHICS_NS_BEGIN

namespace
{
/**
 * @brief Adds @p define to @p source just after its `#version` directive.
 *
 * NOT a plain prepend: GLSL requires the version directive to come first, so a `#define` put in
 * front of it is a parse error — and a parse error here is quiet, because the backend treats
 * "no compiled stage" as "this preset has no SDK program" and shades with its fallback instead.
 * That is exactly how the flat preset first came out unlit-but-bright (vsg's flat set draws the
 * material colour): the define never took effect.
 *
 * @param source GLSL source whose first line is the version directive.
 * @param define Define to insert (without a trailing newline).
 * @return The source with the define on its own line, after the version directive.
 */
std::u8string withDefine(std::u8string_view source, std::u8string_view define)
{
    const auto newline = source.find(u8'\n');
    if (newline == std::u8string_view::npos) {
        return std::u8string(source) + u8"\n" + std::u8string(define) + u8"\n";
    }
    std::u8string out(source.substr(0, newline));
    out += u8"\n";
    out += define;
    out += u8"\n";
    out += source.substr(newline + 1u);
    return out;
}

/**
 * @brief Builds a two-stage (vertex + fragment) program from generated sources.
 *
 * @param name Program name, used by backend diagnostics.
 * @param vertex_source Vertex stage source.
 * @param fragment_source Fragment stage source.
 * @return The program.
 */
intrusive_ptr<ShaderProgram> makeProgram(const char8_t* name, std::u8string_view vertex_source,
                                         std::u8string_view fragment_source)
{
    auto program = make_intrusive<ShaderProgram>();
    program->setName(String(name));
    ShaderStage vertex;
    vertex.type   = ShaderStageType::Vertex;
    vertex.source = String(vertex_source);
    program->addStage(vertex);
    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = String(fragment_source);
    program->addStage(fragment);
    return program;
}

/**
 * @brief The GLSL the shadow ABI inserts at a source's declarations marker.
 *
 * Two paths declare it, in DIFFERENT places, because each spends its low bindings on what it reads
 * first: a fullscreen program's source occupies 0..color_count-1 and its depth takes color_count, so
 * the map and its block follow at 5 / 6 for the canonical four-colour G-buffer; a content program
 * declares the material (0), the optional diffuse map (1) and the lights (2), so the pair takes 3 / 4
 * of the same set. The ORDER is fixed rather than "appended in declaration order" so that a shader
 * text can name its bindings at all: a slot number that depends on how many other things a pipeline
 * happens to have cannot be written down (see .ai/design/render-pipeline.md §9).
 *
 * @param set_index     Descriptor set both bindings live in.
 * @param map_binding   Binding the map takes (the block follows it).
 * @param qualify_set   Whether to spell the set out. A fullscreen program's source declares set 0
 *                      implicitly, so it does not; a content program spells every set.
 * @return The declarations, without a trailing newline.
 */
std::u8string shadowBindings(std::uint32_t set_index, std::uint32_t map_binding, bool qualify_set)
{
    const auto digits = [](std::uint32_t value) {
        const std::string text = std::to_string(value);
        return std::u8string(text.begin(), text.end());
    };
    const auto layout = [&](std::uint32_t binding) {
        return qualify_set ? std::u8string(u8"layout(set = ") + digits(set_index) + u8", binding = " + digits(binding) + u8")"
                           : std::u8string(u8"layout(binding = ") + digits(binding) + u8")";
    };
    std::u8string text = layout(map_binding) + u8" uniform sampler2D shadow_map;\n";
    text += (qualify_set ? std::u8string(u8"layout(set = ") + digits(set_index) + u8", binding = " + digits(map_binding + 1u)
                                  + u8", std140) uniform VineShadowBlock\n{\n"
                        : std::u8string(u8"layout(binding = ") + digits(map_binding + 1u)
                              + u8", std140) uniform VineShadowBlock\n{\n");
    text += u8"    mat4 viewToLight;   // view space -> light clip\n";
    text += u8"    vec4 params;        // x = enabled, y = depth bias, z = strength\n";
    text += u8"} shadow;";
    return text;
}

/**
 * @brief The GLSL term the shadow ABI inserts where a light's diffuse term is summed.
 *
 * Deliberately identical for every path that shades a shadow (the deferred lighting program and the
 * forward content program): the two differ in how they obtain the fragment's view position, not in
 * what a shadow does to a light, so the text is written once and inserted twice.
 */
constexpr char8_t shadow_term[] =
    u8"        if (shadow.params.x > 0.5)\n"
    u8"        {\n"
    u8"            // Where this fragment lands in the LIGHT's map, and how deep the nearest caster\n"
    u8"            // the map found is. Outside the map's rectangle nothing casts, so the fragment\n"
    u8"            // stays lit: the map only covers what the light camera framed.\n"
    u8"            //\n"
    u8"            // The block's matrix is the SDK's clip convention: x to the right, y UP, z 0 at\n"
    u8"            // the near plane to 1 at the far one. The MAP is a Vulkan image this renderer\n"
    u8"            // rasterised, and BOTH of those axes are inverted there: v = 0 is the TOP row\n"
    u8"            // (world up - the same fact that makes the G-buffer upright), and the depth is\n"
    u8"            // REVERSE-Z (near = 1, far = 0 - see SceneBridgePipeline.h DEPTH CONVENTION).\n"
    u8"            // Getting the conversion wrong is not an error anywhere: the picture simply loses\n"
    u8"            // the sun (a z that never matches) or samples a mirrored texel (a v that lands\n"
    u8"            // somewhere else). Both were measured, one bug at a time, by vsg_backend_selftest's\n"
    u8"            // deferred shadow phase - it is the only gate that can see them.\n"
    u8"            vec4  light_clip = shadow.viewToLight * vec4(pos, 1.0);\n"
    u8"            vec3  light_uv   = light_clip.xyz / light_clip.w;\n"
    u8"            vec2  map_uv     = light_uv.xy * vec2(0.5, -0.5) + 0.5;\n"
    u8"            float frag       = 1.0 - (light_uv.z * 0.5 + 0.5);\n"
    u8"            if (all(greaterThan(map_uv, vec2(0.0))) && all(lessThan(map_uv, vec2(1.0))))\n"
    u8"            {\n"
    u8"                float caster = texture(shadow_map, map_uv).r;\n"
    u8"                // Reverse-Z: being in FRONT of the caster is a LARGER value, so the bias\n"
    u8"                // that keeps a surface out of its own shadow leans the same way.\n"
    u8"                float lit    = (frag + shadow.params.y) >= caster ? 1.0 : 0.0;\n"
    u8"                ndl *= mix(1.0, lit, clamp(shadow.params.z, 0.0, 1.0));\n"
    u8"            }\n"
    u8"        }";

constexpr char8_t shadow_bindings_marker[] = u8"// VINE_SHADOW_BINDINGS";
constexpr char8_t shadow_term_marker[]     = u8"// VINE_SHADOW_TERM";

/**
 * @brief Inserts the shadow ABI at a shipped source's two markers.
 *
 * Each marker stands alone on its own line and is REPLACED by its text (or removed, for a variant
 * that declares nothing), so the program text carries no scaffolding: a program is a text the host
 * can read, and a text with the insertion points left in it reads as somebody's template.
 *
 * @param source       Source to rewrite in place.
 * @param declarations Text to put at the declarations marker (empty: remove it).
 * @param term_text    Text to put at the term marker (empty: remove it).
 * @return true when BOTH markers were found; false means the source lost one (the caller decides
 *         whether that is a refusal or a report - see forwardProgram / deferredLightProgram).
 */
bool insertShadowAbi(std::u8string& source, std::u8string_view declarations, std::u8string_view term_text)
{
    const auto replace_marker = [&source](std::u8string_view marker, std::u8string_view text) {
        const std::size_t at = source.find(marker);
        if (at == std::u8string::npos) {
            return false;   // refused below: a program nobody can build is better than one that lies
        }
        // The marker line goes away with its text: it is a comment, but the program NAME and the
        // program TEXT are what the host sees, and a program carrying scaffolding reads as one.
        const std::size_t line_end = source.find(u8'\n', at);
        source.replace(at, (line_end == std::u8string::npos ? source.size() : line_end + 1u) - at, text);
        return true;
    };
    const bool declarations_ok = replace_marker(shadow_bindings_marker, declarations);
    const bool term_ok         = replace_marker(shadow_term_marker, term_text);
    return declarations_ok && term_ok;
}

}  // namespace

/**
 * @brief The forward fragment source: the shipped file with the shadow ABI at its markers.
 *
 * Both forward programs (smooth and flat) are built from THIS text, which is what keeps the flat
 * preset from becoming a second copy of the shading: it differs by one define and by nothing else.
 * A source that LOST a marker is not refused here, unlike the deferred lighting program: this is the
 * engine's default content program, and taking every drawable in the session down over a comment
 * would be a wild overreaction. The consequence of a lost marker is "no shadow reaches this text",
 * and that is not silent: the backend reports a drawable whose program declares no shadow_map while
 * its pass declared a shadow input (see SceneBridge), and EmbeddedShadersTest asserts both markers
 * against the shipped source.
 *
 * The declarations are the CONTENT path's bindings, not the fullscreen path's: set 0 / 3 for the map
 * and 4 for the block, right after the material (0), the optional diffuse map (1) and the lights (2)
 * that share the set.
 *
 * @return The fragment source every forward program is derived from.
 */
std::u8string forwardFragmentSource()
{
    std::u8string source(shaders::kStdForwardFrag);
    insertShadowAbi(source, shadowBindings(0u, 3u, /*qualify_set*/ true), std::u8string_view(shadow_term));
    return source;
}

intrusive_ptr<ShaderProgram> forwardProgram()
{
    // The forward content program carries the shadow ABI UNCONDITIONALLY (see the .frag: the content
    // set is shared per (target, depth mode) and a session picks the content program once, so a
    // shadowed variant would double that cache for every target and a host's own program would still
    // have no shadowed twin to pick). `shadow.params.x` is the switch the one text takes both paths
    // with, which is what the ABI's params block is for (ShaderAbi.hpp).
    return makeProgram(u8"std_forward", shaders::kStdForwardVert, forwardFragmentSource());
}

intrusive_ptr<ShaderProgram> flatForwardProgram()
{
    // Flat shading is the SAME program with one define: the face normal comes from the screen-space
    // derivatives of the view position instead of the interpolated vertex normal (see the fragment
    // source). Injecting the define here keeps ONE lighting source for both programs — the alternative,
    // a second copy of the lighting, is exactly the kind of duplication that drifts.
    //
    // The define goes into the SOURCE (withDefine) rather than through a backend's compile settings:
    // it is a different program text, which is what a program IS, and it keeps this program's identity
    // independent of any backend's define plumbing.
    return makeProgram(u8"std_forward_flat", shaders::kStdForwardVert,
                       withDefine(forwardFragmentSource(), u8"#define VINE_FLAT 1"));
}

intrusive_ptr<ShaderProgram> gbufferGeometryProgram()
{
    return makeProgram(u8"gbuffer_geometry", shaders::kGbufferGeometryVert, shaders::kGbufferGeometryFrag);
}

intrusive_ptr<ShaderProgram> fullscreenVertexProgram()
{
    auto program = make_intrusive<ShaderProgram>();
    program->setName(u8"fullscreen_triangle");
    ShaderStage vertex;
    vertex.type   = ShaderStageType::Vertex;
    vertex.source = String(shaders::kFullscreenVert);
    program->addStage(vertex);
    return program;
}

intrusive_ptr<ShaderProgram> screenCopyProgram(int attachment)
{
    // The attachment is the sampler's BINDING in the fragment text (see the header): rewrite the one
    // line that states it. The marker below is asserted against the shipped source by
    // EmbeddedShadersTest, so an edit to the .frag cannot silently turn every copy into a copy of
    // attachment 0 — the failure mode this substitution would otherwise hide.
    constexpr char8_t marker[] = u8"layout(binding = 0) uniform sampler2D screen_tex;";
    std::u8string     source(shaders::kScreenCopyFrag);
    // The integer goes in as ASCII digits; the digits are the same in char8_t.
    const std::string digits   = std::to_string(attachment);
    const std::u8string attachment_text(digits.begin(), digits.end());
    if (attachment != 0) {
        const std::size_t at = source.find(marker);
        if (at != std::u8string::npos) {
            const std::u8string line =
                std::u8string(u8"layout(binding = ") + attachment_text + u8") uniform sampler2D screen_tex;";
            source.replace(at, sizeof(marker) - 1u, line);
        }
    }

    auto program = make_intrusive<ShaderProgram>();
    program->setName(attachment == 0 ? String(u8"screen_copy") : String(u8"screen_copy_") + String(attachment_text));
    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = String(source);
    program->addStage(fragment);
    return program;
}

intrusive_ptr<ShaderProgram> deferredLightProgram(bool with_shadow)
{
    auto program = make_intrusive<ShaderProgram>();
    program->setName(with_shadow ? u8"deferred_light_shadowed" : u8"deferred_light");
    // The unshadowed variant inserts NOTHING where the shadowed one inserts its bindings and its
    // term: both go through the same substitution, so neither program text carries scaffolding —
    // and a source that lost a marker is refused for both instead of only for one.
    // The shadowed variant differs in TWO places of the source, each marked by a comment the
    // shipped text carries (see the .frag): the declarations, and the factor that scales the
    // light's diffuse term. Both are ASSERTED against the source by EmbeddedShadersTest, so an
    // edit to the .frag that moves or renames a marker fails a test instead of silently
    // producing a program whose shadow terms are somebody else's code.
    //
    // The binding numbers are the shadow ABI (see .ai/design/render-pipeline.md §9): a fullscreen
    // program's source occupies 0..color_count-1, its source's depth takes color_count when that
    // one is sampleable, and the shadow map and its block follow at color_count+1 and +2 — 5 and
    // 6 for the canonical 4-colour G-buffer this program is written against.
    std::u8string     source(shaders::kDeferredLightFrag);
    const std::u8string declarations = with_shadow ? shadowBindings(0u, 5u, /*qualify_set*/ false) : std::u8string{};
    if (!insertShadowAbi(source, declarations, with_shadow ? std::u8string_view(shadow_term) : std::u8string_view{})) {
        return {};   // declined: the caller reports and draws nothing (no silent stand-in)
    }

    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = String(source);
    program->addStage(fragment);
    return program;
}

V_GRAPHICS_NS_END
