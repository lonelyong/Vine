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
    # 1). The backend used to raise that cap because a Qt surface recreation
    # made it tear its vsg::Window/Device down and build a second one while the
    # first was still dying. It no longer has to: a recreated host window MOVES
    # the session (see VsgHostWindow), so no second device is ever created -- and
    # the default cap is what keeps that honest, because a path that did try to
    # create one now throws instead of quietly working on this machine.
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
#
# VsgHostWindow creates the host surface itself, on its own window-system connection (it adopts the
# window the host announces and never destroys it). vsg builds the same code but links xcb PRIVATE, so
# its include directories do not reach this target.
set(_gfx_backend_vsg_host_surface_libs "")
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    if(NOT TARGET PkgConfig::xcb)
        pkg_check_modules(xcb REQUIRED IMPORTED_TARGET xcb)
    endif()
    set(_gfx_backend_vsg_host_surface_libs PkgConfig::xcb)
endif()

target_link_libraries(${GFX_BACKEND_VSG_TARGET} PRIVATE
    vi::Appfw
    vi::Graphics
    vi::Imaging
    vi::Logging
    vsg::vsg
    # The meta-loader this backend reaches extension commands through (see VsgVulkanEntryPoints): the
    # loader exports no symbol for those, and volk is what loads them without naming each one.
    tp::volk
    ${_gfx_backend_vsg_host_surface_libs})

# The backend has no GLSL of its own: every stage it compiles comes from the SDK
# (vine/graphics/EmbeddedShaders.hpp, via BuiltinShaders). See cmake/VineShaders.cmake.

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
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_support.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_pixels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_textures.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_shadows.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_hostsurface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_datarefresh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vsg_selftest/selftest_resize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneBridgeGeometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneBridgePipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CameraBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgMaterialManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgTextureCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgMeshResourceCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgDrawBlockPool.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgProgramSlot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgLights.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgBackendUtility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgDynamicState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgVulkanEntryPoints.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgHostWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VsgPipelineFactory.cpp)
target_compile_definitions(vsg_backend_selftest PRIVATE V_VSG_LIB)
# MSVC options for the project's own code, as v_add_library/v_add_plugin set them: the
# self-test reuses the plugin's translation units, whose string literals carry non-ASCII
# text that MSVC would otherwise read through the system code page (C4819, and C2001 once a
# mis-decoded byte eats the closing quote).
if(MSVC)
    target_compile_options(vsg_backend_selftest PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
endif()
target_include_directories(vsg_backend_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vsg_backend_selftest PRIVATE vi::Graphics vi::Logging vsg::vsg tp::volk
    ${_gfx_backend_vsg_host_surface_libs})
set_target_properties(vsg_backend_selftest PROPERTIES FOLDER third_party)
