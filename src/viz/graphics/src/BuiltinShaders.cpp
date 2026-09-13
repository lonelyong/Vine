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
}  // namespace

intrusive_ptr<ShaderProgram> forwardProgram()
{
    return makeProgram(u8"std_forward", shaders::kStdForwardVert, shaders::kStdForwardFrag);
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
                       withDefine(shaders::kStdForwardFrag, u8"#define VINE_FLAT 1"));
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
    constexpr char8_t bindings[] =
        u8"layout(binding = 5) uniform sampler2D shadow_map;\n"
        u8"layout(binding = 6, std140) uniform VineShadowBlock\n"
        u8"{\n"
        u8"    mat4 viewToLight;   // view space -> light clip\n"
        u8"    vec4 params;        // x = enabled, y = depth bias, z = strength\n"
        u8"} shadow;";
    constexpr char8_t term[] =
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
    constexpr char8_t bindings_marker[] = u8"// VINE_SHADOW_BINDINGS";
    constexpr char8_t term_marker[]     = u8"// VINE_SHADOW_TERM";
    std::u8string     source(shaders::kDeferredLightFrag);
    const auto        replace_marker = [&source](std::u8string_view marker, std::u8string_view text) {
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
    if (!replace_marker(bindings_marker, with_shadow ? bindings : std::u8string_view{}) ||
        !replace_marker(term_marker, with_shadow ? term : std::u8string_view{})) {
        return {};   // declined: the caller reports and draws nothing (no silent stand-in)
    }

    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = String(source);
    program->addStage(fragment);
    return program;
}

V_GRAPHICS_NS_END
