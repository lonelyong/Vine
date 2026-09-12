# gfx_backend_vsg: VSG render backend delivered as an appfw plugin.
#
# The plugin is a MODULE DLL (v_add_plugin) containing the full implementation:
# SceneBridge, CameraBridge, VsgRenderer, VsgMaterialManager and the backend
# factory. When loaded by PluginManager, GfxBackendVsgPlugin::load() registers
# the "vsg" backend into RenderBackendRegistry.
#
# Tests cannot link a MODULE library; test_vsg compiles the implementation
# sources directly (see tests/test_vsg/CMakeLists.txt).

v_add_plugin(GFX_BACKEND_VSG_TARGET gfx_backend_vsg)

# v_add_plugin derives the export macro from the short name (gfx_backend_vsg ->
# V_GFX_BACKEND_VSG_LIB), but vsg_global.hpp checks V_VSG_LIB. Define it
# explicitly so V_VSG_API expands to dllexport when building this plugin.
target_compile_definitions(${GFX_BACKEND_VSG_TARGET} PRIVATE V_VSG_LIB)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
if(VINE_USE_FETCHCONTENT)
    # Static third-party libs (glslang, vsg) are baked into the plugin DLL.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    # Runtime GLSL -> SPIR-V compilation (vsg ShaderCompiler) requires glslang,
    # which vsg discovers itself via find_package(glslang). Quiet find first so
    # an already-installed glslang (system / vcpkg) is reused instead of being
    # downloaded; when none is available, build glslang from source here.
    if(glslang_DIR STREQUAL "${CMAKE_CURRENT_BINARY_DIR}/glslang-config")
        unset(glslang_DIR CACHE)   # Drop the stale shim from a previous configure.
    endif()
    find_package(glslang CONFIG QUIET)
    if(NOT glslang_FOUND)
        FetchContent_Declare(glslang
            GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
            GIT_TAG        16.2.0
        )
        # Only the libraries vsg links against are needed; skip glslang's own
        # tests, install rules, validator binary and SPIRV-Tools optimizer.
        set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
        set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(glslang)
        v_group_targets_folder(${glslang_SOURCE_DIR} third_party)
        # glslang now lives in this build tree; its static targets must be
        # folded into vsg's export set and its headers mirrored into the
        # installed layout (see below) before vsg is built.
        set(VINE_VSG_GLSLANG_IN_TREE TRUE)

        # An installed glslang ships its SPIRV headers as include/glslang/SPIRV,
        # the layout vsg's ShaderCompiler.cpp expects. In-tree glslang keeps
        # them at <src>/SPIRV instead, so mirror them into the build tree and
        # expose the mirror on the SPIRV target below.
        set(_glslang_include_mirror "${CMAKE_CURRENT_BINARY_DIR}/glslang-include")
        file(MAKE_DIRECTORY "${_glslang_include_mirror}/glslang/SPIRV")
        file(GLOB _glslang_spirv_headers
            "${glslang_SOURCE_DIR}/SPIRV/*.h"
            "${glslang_SOURCE_DIR}/SPIRV/spirv.hpp11")
        file(COPY ${_glslang_spirv_headers}
            DESTINATION "${_glslang_include_mirror}/glslang/SPIRV")

        # glslang's glslang::* targets are project-visible once the subproject
        # is added, but vsg re-runs find_package(glslang CONFIG QUIET) during
        # its own configure. Generate a minimal config that confirms the
        # in-tree glslang and point glslang_DIR at it, so vsg keeps its
        # ShaderCompiler (VSG_SUPPORTS_ShaderCompiler) enabled.
        set(_glslang_config_dir "${CMAKE_CURRENT_BINARY_DIR}/glslang-config")
        file(MAKE_DIRECTORY "${_glslang_config_dir}")
        file(WRITE "${_glslang_config_dir}/glslang-config.cmake"
            "if(NOT TARGET glslang::glslang)\n"
            "    message(FATAL_ERROR \"in-tree glslang is missing its glslang::glslang target\")\n"
            "endif()\n"
            "set(glslang_FOUND TRUE)\n")
        set(glslang_DIR "${_glslang_config_dir}" CACHE PATH "" FORCE)
    endif()

    # A prebuilt no-glslang VSG (e.g. the local /opt/opensrc/VSG install)
    # cannot provide runtime shader compilation, so VSG is always built from
    # source here; the glslang above (installed or fetched) enables its
    # ShaderCompiler. The embedded precompiled phong SPIR-V keeps the default
    # (no user Program) path rendering correctly regardless.
    FetchContent_Declare(vsg
        GIT_REPOSITORY https://github.com/vsg-dev/VulkanSceneGraph.git
        GIT_TAG        v1.1.16
    )
    set(VSG_SUPPORTS_ShaderCompiler ON CACHE BOOL "" FORCE)
    # vsg caps simultaneous vsg::Device instances at VSG_MAX_DEVICES (default
    # 1). Qt surface recreation makes the backend tear down and recreate its
    # vsg::Window/Device, so raise the cap to let a fresh device coexist with
    # a dying one instead of tripping vsg's Device.cpp exception.
    set(VSG_MAX_DEVICES 4 CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(vsg)
    v_group_targets_folder(${vsg_SOURCE_DIR} third_party)

    if(VINE_VSG_GLSLANG_IN_TREE)
        # vsg registers install/export of its vsgTargets export set during its
        # own configure. As a static library, vsg re-exports the PRIVATE
        # glslang link dependencies through that set, and CMake requires each
        # referenced non-imported target to be a member of it. Fold the fetched
        # glslang static targets into vsgTargets so generation succeeds. Vine
        # consumes vsg in-tree and never installs it, so these rules are inert.
        install(TARGETS glslang glslang-default-resource-limits SPIRV
                EXPORT vsgTargets
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        # vsg's ShaderCompiler.cpp includes <glslang/SPIRV/GlslangToSpv.h>, so
        # expose the mirrored SPIRV headers through glslang::SPIRV's interface.
        # SPIRV is folded into vsg's export set above, so the path must be
        # wrapped in a BUILD_INTERFACE generator expression to stay relocatable.
        target_include_directories(SPIRV PUBLIC "$<BUILD_INTERFACE:${_glslang_include_mirror}>")
    endif()
else()
    # Consume vsg from vcpkg or a local install (e.g. /opt/opensrc/VSG).
    # NOTE: a local install built without glslang disables runtime shader
    # compilation (vsg ShaderCompiler); use VINE_USE_FETCHCONTENT=ON for a
    # glslang-enabled vsg.
    find_package(vsg CONFIG REQUIRED)
endif()

# vsg::vsg is PUBLIC because SceneBridge exposes vsg::ref_ptr<vsg::Node> in its API.
# The plugin needs appfw for the Plugin base class, graphics for the backend
# interface + registry and logging for its informational traces.
target_link_libraries(${GFX_BACKEND_VSG_TARGET} PRIVATE
    vi::Appfw
    vi::Graphics
    vi::Logging
    vsg::vsg)

# TEMP diagnostic: standalone minimal vsg executable that renders a red box
# through vsg's own Builder/phong path, to isolate whether the vendored vsg
# renders correct colors independent of Vine. Remove after diagnosis.
add_executable(vsg_color_probe ${CMAKE_CURRENT_SOURCE_DIR}/vsg_probe/main.cpp)
target_link_libraries(vsg_color_probe PRIVATE vsg::vsg)
set_target_properties(vsg_color_probe PROPERTIES FOLDER third_party)

# TEMP diagnostic: dump the embedded phong ShaderSet SPIR-V for inspection.
# Remove after diagnosis.
add_executable(vsg_shader_dump ${CMAKE_CURRENT_SOURCE_DIR}/vsg_shader_dump/main.cpp)
target_link_libraries(vsg_shader_dump PRIVATE vsg::vsg)
set_target_properties(vsg_shader_dump PROPERTIES FOLDER third_party)

# GPU-free (lavapipe) end-to-end self-test of the vsg backend GPU paths that
# device-free unit tests cannot reach: off-screen MRT targets, PiP sampling,
# deferred fullscreen programs, multi-pass sharing of camera/target/scene/
# viewport, hot edits and resource-release teardown. It validates "no crash +
# no Vulkan validation-layer error"; pixel-level visuals need a real GPU.
# Run headless under the lavapipe ICD with the validation layer enabled, e.g.
#   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
#   VINE_VSG_DEBUG_LAYER=1 VINE_SELFTEST_FRAMES=30 ./vsg_backend_selftest
add_executable(vsg_backend_selftest
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneBridgeGeometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneBridgePipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CameraBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgMaterialManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgSceneRules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgDiagnostics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgRecordOrder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgReadback.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgViewCompiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgContentSlot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgTargetBookkeeping.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgPassMaterialiser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgRetireRing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgRendererPasses.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgOverlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgBackendUtility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgPipelineFactory.cpp)
target_compile_definitions(vsg_backend_selftest PRIVATE V_VSG_LIB)
target_include_directories(vsg_backend_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vsg_backend_selftest PRIVATE vi::Graphics vi::Logging vsg::vsg)
set_target_properties(vsg_backend_selftest PROPERTIES FOLDER third_party)
