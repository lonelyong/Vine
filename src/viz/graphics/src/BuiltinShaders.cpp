#include <vine/graphics/BuiltinShaders.hpp>

#include <string_view>

#include <vine/graphics/EmbeddedShaders.hpp>

V_GRAPHICS_NS_BEGIN

namespace
{
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

intrusive_ptr<ShaderProgram> builtinProgram(ShaderPreset preset)
{
    switch (preset)
    {
    case ShaderPreset::StandardPhong:
        return makeProgram(u8"vine_forward", shaders::kVineForwardVert, shaders::kVineForwardFrag);
    case ShaderPreset::FlatShaded:
    case ShaderPreset::Pbr:
    case ShaderPreset::ShadowedPhong:
        break;
    }
    // No SDK shading for this preset yet: the caller keeps its own fallback.
    return {};
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
