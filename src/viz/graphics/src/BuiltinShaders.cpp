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
    return makeProgram(u8"vine_forward", shaders::kVineForwardVert, shaders::kVineForwardFrag);
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
    return makeProgram(u8"vine_flat", shaders::kVineForwardVert,
                       withDefine(shaders::kVineForwardFrag, u8"#define VINE_FLAT 1"));
}

intrusive_ptr<ShaderProgram> gbufferGeometryProgram()
{
    return makeProgram(u8"gbuffer_geometry", shaders::kGbufferGeometryVert, shaders::kGbufferGeometryFrag);
}

intrusive_ptr<ShaderProgram> deferredLightProgram()
{
    auto program = make_intrusive<ShaderProgram>();
    program->setName(u8"deferred_light");
    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = String(shaders::kDeferredLightFrag);
    program->addStage(fragment);
    return program;
}

V_GRAPHICS_NS_END
