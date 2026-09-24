# gfx_backend_vsg: VSG render backend delivered as an appfw plugin.
#
# The plugin is a MODULE DLL (vn_add_plugin). Its implementation is the rewritten backend under src/api
# and src/core, plus the handful of flat translation units those share (the host window, the scene rules,
# the vulkan entry points, the dynamic-state mapper and the backend utility layer - see the source list
# in tests/test_vsg/CMakeLists.txt). When loaded by PluginManager, GfxBackendVsgPlugin::load() registers
# the "vsg" backend into RenderBackendRegistry.
#
# The retired renderer (VsgRenderer and everything it drove: SceneBridge, CameraBridge, the texture and
# mesh caches, the pipeline factory, the retire ring, ...) has been deleted; its last caller was gone once
# the factory created VsgBackend. What replaces its end-to-end evidence is the application gate in
# scripts/vsg_rewrite_gate.sh.
#
# Tests cannot link a MODULE library; test_vsg compiles the implementation
# sources directly (see tests/test_vsg/CMakeLists.txt).

vn_add_plugin(GFX_BACKEND_VSG_TARGET gfx_backend_vsg)

# vn_add_plugin derives the export macro from the short name (gfx_backend_vsg ->
# VN_GFX_BACKEND_VSG_LIB), but vsg_global.hpp checks VN_VSG_LIB. Define it
# explicitly so VN_VSG_API expands to dllexport when building this plugin.
target_compile_definitions(${GFX_BACKEND_VSG_TARGET} PRIVATE VN_VSG_LIB)

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
        vn_group_targets_folder(${glslang_SOURCE_DIR} third_party)
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
    vn_group_targets_folder(${vsg_SOURCE_DIR} third_party)

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
    vn::Appfw
    vn::Graphics
    vn::Imaging
    vn::Logging
    vsg::vsg
    # The meta-loader this backend reaches extension commands through (see VsgVulkanEntryPoints): the
    # loader exports no symbol for those, and volk is what loads them without naming each one.
    tp::volk
    ${_gfx_backend_vsg_host_surface_libs})

# The backend has no GLSL of its own: every stage it compiles comes from the SDK
# (vine/graphics/EmbeddedShaders.hpp, via BuiltinShaders). See cmake/VineShaders.cmake.

# The GPU-free end-to-end self-test target (vsg_backend_selftest) USED TO BE HERE. It was built from
# the retired renderer's own translation units (SceneBridge, CameraBridge, VsgRenderer, ...) and drove
# that renderer through its off-screen/MRT/PiP/deferred phases - all of code that no longer runs. The
# application gate in scripts/vsg_rewrite_gate.sh is the end-to-end evidence now: it starts the demo on
# lavapipe with the validation layer, samples the picture before and after a resize, and fails on any
# unknown VUID.
