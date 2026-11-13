# glslang: ONE provisioning point for every target that compiles GLSL to SPIR-V at run time.
#
# Two backends compile shaders: gfx_backend_vsg (through vsg's ShaderCompiler) and gfx_backend_vk (directly, through
# glslang's own TShader/TProgram). The decision - "an installed glslang if the machine has one, otherwise build it with
# FetchContent" - has to be made ONCE and BEFORE either plugin configures. When gfx_backend_vsg owned it, the vk
# backend could link glslang::glslang only because that plugin happened to be configured first, which is a dependency
# on subdirectory order rather than a stated one (and a build that dropped the vsg plugin had no glslang at all).
#
# vn_provide_glslang() is idempotent: it returns at once when glslang::glslang already exists, so the vsg plugin may
# call it again for the record without re-fetching anything.

# Maps the intermediate configurations onto RELEASE for an INSTALLED glslang (vcpkg exports only DEBUG and RELEASE, so
# a RelWithDebInfo / MinSizeRel consumer would otherwise fall back to DEBUG and link a debug-runtime glslang into a
# release-runtime target - LNK2038 and an unresolved __imp__CrtDbgReport on Windows).
function(_vn_glslang_map_imported_configs)
    foreach(_vn_glslang_config RelWithDebInfo MinSizeRel)
        string(TOUPPER "${_vn_glslang_config}" _vn_glslang_config_upper)
        foreach(_vn_glslang_target
                glslang::glslang
                glslang::SPIRV
                glslang::glslang-default-resource-limits)
            if(TARGET ${_vn_glslang_target})
                get_target_property(_vn_glslang_imported ${_vn_glslang_target} IMPORTED)
                if(_vn_glslang_imported)
                    set_property(TARGET ${_vn_glslang_target}
                                 PROPERTY MAP_IMPORTED_CONFIG_${_vn_glslang_config_upper} RELEASE)
                endif()
            endif()
        endforeach()
    endforeach()
endfunction()

function(vn_provide_glslang)
    if(TARGET glslang::glslang)
        return()
    endif()

    # A generated shim left behind by an older configure points into a directory this build no longer populates, and
    # finding it again would end in the shim's own FATAL_ERROR. Drop it and search afresh.
    if(glslang_DIR MATCHES "glslang-config$")
        unset(glslang_DIR CACHE)
    endif()

    include(FetchContent)
    if(NOT VN_USE_FETCHCONTENT)
        # Third-party dependencies come from vcpkg / a system install: glslang must be one of them, because both
        # shader-compiling backends need it and there is nothing to download here.
        find_package(glslang CONFIG REQUIRED)
        _vn_glslang_map_imported_configs()
        return()
    endif()

    # An already-installed glslang (system / vcpkg) is reused instead of being downloaded.
    find_package(glslang CONFIG QUIET)
    if(glslang_FOUND)
        _vn_glslang_map_imported_configs()
        return()
    endif()

    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG        16.2.0
    )
    # Static third-party libs are baked into the plugins' binaries (the vsg plugin states the same for vsg itself);
    # doing it before the fetch keeps a fresh tree's glslang build shape identical to the previous arrangement.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Only the libraries the backends link against are needed; skip glslang's own tests, install rules, validator
    # binary and SPIRV-Tools optimizer.
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glslang)
    # Cosmetic (IDE folders). The provisioning runs at the TOP level, before src/ includes VineLibraryHelper, so the
    # helper may not exist yet - a folder that cannot be grouped is not a reason to fail the configure.
    if(COMMAND vn_group_targets_folder)
        vn_group_targets_folder(${glslang_SOURCE_DIR} third_party)
    endif()

    # An installed glslang ships its SPIRV headers as include/glslang/SPIRV, the layout vsg's ShaderCompiler.cpp
    # expects; in-tree glslang keeps them at <src>/SPIRV instead. Mirror them into the build tree and expose the mirror
    # on glslang::SPIRV, whose include directories are what both consumers compile against.
    set(_vn_glslang_include_mirror "${CMAKE_BINARY_DIR}/glslang-include")
    file(MAKE_DIRECTORY "${_vn_glslang_include_mirror}/glslang/SPIRV")
    file(GLOB _vn_glslang_spirv_headers
        "${glslang_SOURCE_DIR}/SPIRV/*.h"
        "${glslang_SOURCE_DIR}/SPIRV/spirv.hpp11")
    file(COPY ${_vn_glslang_spirv_headers}
        DESTINATION "${_vn_glslang_include_mirror}/glslang/SPIRV")
    target_include_directories(SPIRV PUBLIC "$<BUILD_INTERFACE:${_vn_glslang_include_mirror}>")
    set(VN_GLSLANG_INCLUDE_MIRROR "${_vn_glslang_include_mirror}" CACHE INTERNAL "in-tree glslang SPIRV header mirror")

    # vsg re-runs find_package(glslang CONFIG QUIET) during its own configure, to decide whether to enable its
    # ShaderCompiler. Point glslang_DIR at a minimal config that confirms the in-tree targets.
    set(_vn_glslang_config_dir "${CMAKE_BINARY_DIR}/glslang-config")
    file(MAKE_DIRECTORY "${_vn_glslang_config_dir}")
    file(WRITE "${_vn_glslang_config_dir}/glslang-config.cmake"
        "if(NOT TARGET glslang::glslang)\n"
        "    message(FATAL_ERROR \"in-tree glslang is missing its glslang::glslang target\")\n"
        "endif()\n"
        "set(glslang_FOUND TRUE)\n")
    set(glslang_DIR "${_vn_glslang_config_dir}" CACHE PATH "" FORCE)

    set(VN_GLSLANG_IN_TREE TRUE CACHE INTERNAL "glslang was fetched into this build tree")
endfunction()
