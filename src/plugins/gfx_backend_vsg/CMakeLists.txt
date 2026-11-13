# gfx_backend_vsg: VSG render backend delivered as an appfw plugin.
#
# The plugin is a MODULE DLL (vn_add_plugin). Its implementation is the rewritten backend under src/internal
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
# VN_GFX_BACKEND_VSG_LIB), but backend_vsg_global.hpp checks VN_BACKEND_VSG_LIB. Define it
# explicitly so VN_BACKEND_VSG_API expands to dllexport when building this plugin.
target_compile_definitions(${GFX_BACKEND_VSG_TARGET} PRIVATE VN_BACKEND_VSG_LIB)

# This plugin consumes the backend half of graphics (`vine/graphics/backend/`), so it opts in
# (see that half's BackendHalfGuard.hpp).
target_compile_definitions(${GFX_BACKEND_VSG_TARGET} PRIVATE VN_GRAPHICS_BACKEND_CONSUMER)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Static third-party libs (glslang, vsg) are baked into the plugin DLL.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# glslang is provisioned ONCE for every shader-compiling backend, at the TOP LEVEL before any plugin configures
# (see cmake/VineGlslang.cmake). vsg discovers it afterwards through the same glslang_DIR that helper left behind;
# calling the helper here again is a no-op and states the dependency.
vn_provide_glslang()

include(FetchContent)
if(VN_USE_FETCHCONTENT)
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
    # vsg's vk_buffer is templated on VSG_MAX_DEVICES and picks one of three shapes: <=1 a single value (no
    # indirection at all), <=4 a FIXED std::array, and >4 a std::vector that grows on the WRITE path only
    # (its const operator[] reads whatever was written, and size() reports what was written, so a read of a
    # slot nobody wrote walks off the end - the branch is broken in vsg 1.1.16 and picking it aborted
    # test_vsg). This build therefore takes the FIXED shape, and with it vsg's own cap on simultaneous
    # vsg::Device instances: its Device ctor throws past VSG_MAX_DEVICES (see vsg's Device.cpp, and
    # vsg::Device::maxNumDevices() for the number).
    #
    # WHY THAT IS ACCEPTABLE RATHER THAN A LIMIT WE HIDE: one device per render area IS the document model's
    # shape (a view brings its own control - see appfw-document-model.md §9), so the cap is reached by
    # opening one more area than the build allows, and the backend answers THAT by refusing the attach with a
    # reason instead of letting vsg's exception leave the event loop (gfx_backend_vsg/src/internal/Device.cpp
    # + Session.cpp, pinned by test_vsg's
    # VsgBackendTest.ADeviceCapThatIsReachedIsAReasonAndNotAnException).
    set(VSG_MAX_DEVICES 4 CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(vsg)
    vn_group_targets_folder(${vsg_SOURCE_DIR} third_party)

    if(VN_GLSLANG_IN_TREE)
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
        # The mirror is what the shared helper created (see cmake/VineGlslang.cmake);
        # SPIRV is folded into vsg's export set above, so the path must be wrapped
        # in a BUILD_INTERFACE generator expression to stay relocatable.
        target_include_directories(SPIRV PUBLIC "$<BUILD_INTERFACE:${VN_GLSLANG_INCLUDE_MIRROR}>")
    endif()
else()
    # Consume vsg from vcpkg or a local install (e.g. /opt/opensrc/VSG).
    # NOTE: a local install built without glslang disables runtime shader
    # compilation (vsg ShaderCompiler); use VN_USE_FETCHCONTENT=ON for a
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
