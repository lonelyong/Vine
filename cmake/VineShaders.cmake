# VineShaders.cmake — the repository's shader inventory.
#
# Every GLSL source that is embedded into a binary is declared here, once, with
# the header it is generated into. Declaring the rules at the top level is what
# lets targets in unrelated directories (e.g. tests/test_vsg, which compiles the
# vsg backend plugin's sources directly) depend on the same generated header.
#
# Two owners exist today:
#   * vine/graphics/EmbeddedShaders.hpp — the SDK's deferred-shading programs,
#     used by RenderPipelineBuilder (src/viz/graphics/shaders/).
#   * vine/vsg/EmbeddedShaders.hpp — the vsg backend's own stages, used by
#     VsgPipelineFactory (src/plugins/gfx_backend_vsg/shaders/).
#
# Adding a shader: drop the .vert/.frag in the owning shaders/ directory, add it
# to the SOURCES list below, include the generated header, and use the generated
# constant named after the file (gbuffer_geometry.vert -> kGbufferGeometryVert).
# scripts/vine_shader_check.sh validates every shader listed here.

include(VineShaderHelper)

set(VINE_SDK_SHADER_DIR "${CMAKE_SOURCE_DIR}/src/viz/graphics/shaders")
set(VINE_VSG_SHADER_DIR "${CMAKE_SOURCE_DIR}/src/plugins/gfx_backend_vsg/shaders")

v_declare_embedded_shaders(
    OUTPUT vine/graphics/EmbeddedShaders.hpp
    NAMESPACE vine::graphics::shaders
    SOURCES
        "${VINE_SDK_SHADER_DIR}/gbuffer_geometry.vert"
        "${VINE_SDK_SHADER_DIR}/gbuffer_geometry.frag"
        "${VINE_SDK_SHADER_DIR}/deferred_light.frag"
)

v_declare_embedded_shaders(
    OUTPUT vine/vsg/EmbeddedShaders.hpp
    NAMESPACE vine::vsg::shaders
    SOURCES
        "${VINE_VSG_SHADER_DIR}/fullscreen.vert"
        "${VINE_VSG_SHADER_DIR}/screen_texture.frag"
)
