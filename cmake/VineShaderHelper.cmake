# VineShaderHelper.cmake — embed GLSL sources into a target's binary.
#
# Shaders live as real .vert/.frag files next to the code that owns them, and are
# turned into a generated header of string constants at build time. Nothing is
# copied at runtime and nothing is committed as pre-compiled SPIR-V.
#
# Two-phase API, because a generated file may only be produced once yet consumed
# by targets in other directories (e.g. test_vsg compiles the plugin's sources):
#
#   # Once, at the top level (the repository root, whose scope covers both src/
#   # and tests/):
#   v_declare_embedded_shaders(OUTPUT vine/graphics/EmbeddedShaders.hpp
#                              NAMESPACE vine::graphics::shaders
#                              SOURCES src/viz/graphics/shaders/builtin_forward.vert
#                                      src/viz/graphics/shaders/builtin_forward.frag)
#
#   # In any directory, for every target that compiles a TU including the header:
#   v_use_embedded_shaders(<target> OUTPUT vine/vsg/EmbeddedShaders.hpp)
#
# The declaration must be at the top level: a custom command is only visible in
# the directory that declares it and its subdirectories, so declaring it inside
# the owning plugin would make it invisible to tests/, and the test build could
# race the header's generation.
#
# Constant names are derived from the file name (builtin_gbuffer.vert ->
# kBuiltinGbufferVert) so the C++ side is predictable, and the include path is
# <vine/...> below the build tree's generated/ directory.

include_guard(GLOBAL)

# Generated headers live here; consumers get this on their include path.
set(VINE_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")

# v_declare_embedded_shaders — declares the generation rule for one header.
#
# @param OUTPUT    Header path below the generated include dir, used verbatim as
#                  the include spelling (e.g. vine/vsg/EmbeddedShaders.hpp).
# @param NAMESPACE C++ namespace of the generated constants.
# @param SOURCES   Shader sources; relative paths are resolved against the
#                  calling directory.
function(v_declare_embedded_shaders)
    cmake_parse_arguments(ARG "" "OUTPUT;NAMESPACE" "SOURCES" ${ARGN})
    if(NOT ARG_OUTPUT)
        message(FATAL_ERROR "v_declare_embedded_shaders: OUTPUT is required")
    endif()
    if(NOT ARG_NAMESPACE)
        message(FATAL_ERROR "v_declare_embedded_shaders: NAMESPACE is required")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "v_declare_embedded_shaders: SOURCES is required")
    endif()

    get_property(declared GLOBAL PROPERTY VINE_EMBEDDED_SHADERS_DECLARED)
    if(ARG_OUTPUT IN_LIST declared)
        message(FATAL_ERROR
            "v_declare_embedded_shaders: ${ARG_OUTPUT} is already declared")
    endif()

    set(header "${VINE_GENERATED_INCLUDE_DIR}/${ARG_OUTPUT}")
    set(sources "")
    foreach(source IN LISTS ARG_SOURCES)
        if(NOT IS_ABSOLUTE "${source}")
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR
                "v_declare_embedded_shaders: shader source not found: ${source}")
        endif()
        list(APPEND sources "${source}")
    endforeach()

    # The generator runs in script mode; a ','-joined list reaches it as ONE
    # argument (a ';' list would be split by CMake's list expansion).
    string(REPLACE ";" "," packed_sources "${sources}")
    string(MAKE_C_IDENTIFIER "${ARG_OUTPUT}" rule_suffix)

    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}"
                "-DOUT=${header}"
                "-DSOURCES=${packed_sources}"
                "-DNAMESPACE=${ARG_NAMESPACE}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/v_embed_shaders.cmake"
        DEPENDS ${sources} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/v_embed_shaders.cmake"
        COMMENT "Embedding shaders into ${ARG_OUTPUT}"
        VERBATIM
    )

    add_custom_target(vine_embed_shaders_${rule_suffix} DEPENDS "${header}")

    set_property(GLOBAL APPEND PROPERTY VINE_EMBEDDED_SHADERS_DECLARED "${ARG_OUTPUT}")
    set_property(GLOBAL APPEND PROPERTY VINE_EMBEDDED_SHADER_SOURCES ${sources})
endfunction()

# v_use_embedded_shaders — wires a target that includes a declared header.
#
# @param target  Target to wire (must be in the declaring directory or below).
# @param OUTPUT  Header path as passed to v_declare_embedded_shaders.
# @param SOURCES Optional keyword: also lists the generated header in the
#                target's sources so IDEs show it (default: ordering only).
function(v_use_embedded_shaders target)
    cmake_parse_arguments(ARG "SOURCES" "OUTPUT" "" ${ARGN})
    if(NOT ARG_OUTPUT)
        message(FATAL_ERROR "v_use_embedded_shaders: OUTPUT is required")
    endif()

    get_property(declared GLOBAL PROPERTY VINE_EMBEDDED_SHADERS_DECLARED)
    if(NOT ARG_OUTPUT IN_LIST declared)
        message(FATAL_ERROR
            "v_use_embedded_shaders: ${ARG_OUTPUT} was never declared; "
            "declare it at the top level with v_declare_embedded_shaders")
    endif()

    string(MAKE_C_IDENTIFIER "${ARG_OUTPUT}" rule_suffix)
    set(header "${VINE_GENERATED_INCLUDE_DIR}/${ARG_OUTPUT}")
    target_include_directories(${target} PRIVATE "${VINE_GENERATED_INCLUDE_DIR}")
    add_dependencies(${target} vine_embed_shaders_${rule_suffix})
    if(ARG_SOURCES)
        target_sources(${target} PRIVATE "${header}")
    endif()
endfunction()
