# VineShaders.cmake — the repository's shader inventory.
#
# Every GLSL source that is embedded into a binary is declared here, once, with
# the header it is generated into. Declaring the rules at the top level is what
# lets targets in unrelated directories (e.g. tests/test_vsg, which compiles the
# vsg backend plugin's sources directly) depend on the same generated header.
#
# ONE owner: the SDK owns every GLSL source in the engine.
#
#   * vine/graphics/EmbeddedShaders.hpp — the engine's shading and compositing text, used by
#     BuiltinShaders / RenderPipelineBuilder (src/viz/graphics/shaders/). The engine owns the
#     TEXT; a backend owns how it is compiled and bound (its ABI).
#
# A backend has no shader directory of its own. It used to (src/plugins/gfx_backend_vsg/shaders/,
# a full-screen triangle and a screen-copy fragment stage, generated into
# vine/vsg/EmbeddedShaders.hpp): two stages behind engine-visible pictures — a ScreenPass with no
# program drew the copy, and every full-screen program was written against that triangle — while
# the text lived in one backend. Now both are SDK programs (BuiltinShaders::fullscreenVertexProgram
# / screenCopyProgram) and a backend may only decide HOW to compile and bind them, or provide its
# own stages behind the documented interface.
#
# Adding a shader: drop the .vert/.frag in src/viz/graphics/shaders/, add it to the SOURCES list
# below, include the generated header, and use the generated constant named after the file
# (gbuffer_geometry.vert -> kGbufferGeometryVert).
# scripts/vine_shader_check.sh validates every shader listed here.

include(VineShaderHelper)

set(VINE_SDK_SHADER_DIR "${CMAKE_SOURCE_DIR}/src/viz/graphics/shaders")

v_declare_embedded_shaders(
    OUTPUT vine/graphics/EmbeddedShaders.hpp
    NAMESPACE vine::graphics::shaders
    SOURCES
        "${VINE_SDK_SHADER_DIR}/fullscreen.vert"
        "${VINE_SDK_SHADER_DIR}/screen_copy.frag"
        "${VINE_SDK_SHADER_DIR}/gbuffer_geometry.vert"
        "${VINE_SDK_SHADER_DIR}/gbuffer_geometry.frag"
        "${VINE_SDK_SHADER_DIR}/deferred_light.frag"
        "${VINE_SDK_SHADER_DIR}/vine_forward.vert"
        "${VINE_SDK_SHADER_DIR}/vine_forward.frag"
)
