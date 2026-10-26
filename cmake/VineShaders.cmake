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
# Naming (the rule every file below follows):
#
#   builtin_<role>.<stage>     e.g. builtin_forward.frag, builtin_gbuffer.vert
#
#   * `builtin_` because the file IS the engine's built-in text for that role: the prefix is what
#     separates the text this repository ships from a host's own shader, and every STAGE file here
#     has it (a name is either a built-in or a host's, never ambiguous).
#   * `<role>` is the renderer's word for what the stage does, not a product, vendor or C++ word -
#     a shader file name lives in the renderer's vocabulary (`forward`, `gbuffer`,
#     `deferred_lighting`, `skybox`, `screen_copy`, `fullscreen`), which is also what a reader of
#     any other engine expects to find.
#   * `<stage>` is the stage suffix (`.vert` / `.frag` / ... - see VineShaderHelper.cmake).
#   * No role word is repeated from the stage or from the target: `builtin_gbuffer.frag` writes the
#     G-buffer, it does not need to say "geometry" as well; the ROLE is the pass, and the pass in
#     the built-in pipeline carries the same word (see RenderPipelineBuilder: pass `gbuffer`,
#     `deferred_lighting`).
#
#   vn_<topic>.glsl   e.g. vine_params.glsl
#
#   * A FRAGMENT, not a stage: it has no `#version` and no `main`, it is not embedded on its own,
#     and a shader names it with `#include "vine_params.glsl"` (the build inlines it - see
#     cmake/VineEmbedShaders.cmake). `vn_` rather than `builtin_` because what it holds is the
#     engine's ABI vocabulary (the parameter blocks), not the text of one stage; `.glsl` because
#     the extension names its role in the build: an include, never a compiled stage.
#   * It is declared under INCLUDES below, not SOURCES: the entry is not a program, but the
#     generated header must still depend on it.
#
# Adding a shader: drop the .vert/.frag in src/viz/graphics/shaders/, add it to the SOURCES list
# below, include the generated header, and use the generated constant named after the file
# (builtin_forward.vert -> kBuiltinForwardVert).
# Adding a fragment: drop the .glsl there, list it under INCLUDES, and `#include` it from the
# stages that need it.
# scripts/vn_shader_check.sh validates every shader listed here.

include(VineShaderHelper)

set(VN_SDK_SHADER_DIR "${CMAKE_SOURCE_DIR}/src/viz/graphics/shaders")

vn_declare_embedded_shaders(
    OUTPUT vine/graphics/EmbeddedShaders.hpp
    NAMESPACE vn::graphics::shaders
    SOURCES
        "${VN_SDK_SHADER_DIR}/builtin_fullscreen.vert"
        "${VN_SDK_SHADER_DIR}/builtin_screen_copy.frag"
        "${VN_SDK_SHADER_DIR}/builtin_gbuffer.vert"
        "${VN_SDK_SHADER_DIR}/builtin_gbuffer.frag"
        "${VN_SDK_SHADER_DIR}/builtin_deferred_lighting.frag"
        "${VN_SDK_SHADER_DIR}/builtin_forward.vert"
        "${VN_SDK_SHADER_DIR}/builtin_forward.frag"
        "${VN_SDK_SHADER_DIR}/builtin_skybox.vert"
        "${VN_SDK_SHADER_DIR}/builtin_skybox.frag"
    INCLUDES
        "${VN_SDK_SHADER_DIR}/vine_params.glsl"
        "${VN_SDK_SHADER_DIR}/vine_material.glsl"
        "${VN_SDK_SHADER_DIR}/vine_brdf.glsl"
        "${VN_SDK_SHADER_DIR}/vine_gbuffer.glsl"
)
