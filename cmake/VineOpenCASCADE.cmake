# One OpenCASCADE probe for every module that needs the B-rep backend.
#
# Two modules want it for different reasons — `brep` for runtime operations
# (tessellation), `brepio` for file formats — so the search lives here instead
# of being repeated in both. Include this file from a module's CMakeLists and
# call vn_link_opencascade(); including it twice is harmless, the find result is
# cached and the modules are the same imported targets.
include_guard(GLOBAL)

find_package(OpenCASCADE CONFIG QUIET)

if(OpenCASCADE_FOUND)
    set(VN_HAS_OCCT TRUE CACHE INTERNAL "OpenCASCADE was found")
    # OpenCASCADE publishes its directories as plain variables and relies on directory scope for the include path and for
    # its imported targets, so only the module that happens to run the lookup inherits them - and this file is guarded to
    # run once. Both directories are promoted to the cache here and attached to each target in vn_link_opencascade().
    set(VN_OCCT_INCLUDE_DIR "${OpenCASCADE_INCLUDE_DIR}" CACHE INTERNAL "OpenCASCADE include directory")
    set(VN_OCCT_LIBRARY_DIR "${OpenCASCADE_LIBRARY_DIR}" CACHE INTERNAL "OpenCASCADE library directory")
    set(VN_OCCT_BINARY_DIR "${OpenCASCADE_BINARY_DIR}" CACHE INTERNAL "OpenCASCADE runtime directory")
    message(STATUS "OpenCASCADE found - B-rep backends enabled")
else()
    set(VN_HAS_OCCT FALSE CACHE INTERNAL "OpenCASCADE was found")
    message(STATUS "OpenCASCADE not found - B-rep backends disabled")
endif()

# vn_link_opencascade(<target> <module_macro> [BOOLEANS])
#
# Links the found OpenCASCADE modules PRIVATE and defines <module_macro> on the
# target, so:
# - a consumer of <target> does not inherit the dependency (mesh-only consumers
#   keep not paying for it),
# - the module's own sources guard the backend with `#if defined(<module_macro>)`.
#
# BOOLEANS adds the boolean-operations modules, needed by code that builds one
# solid out of several (for example a capsule as a cylinder fused with two
# spheres). Leave it off otherwise: it is the heaviest part of the backend.
function(vn_link_opencascade target module_macro)
    if(NOT VN_HAS_OCCT)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE ${module_macro})

    # The vcpkg config exports no imported targets: it sets `OpenCASCADE_INCLUDE_DIR` / `OpenCASCADE_LIBRARY_DIR` and
    # leaves the modules as plain library names. Whatever the package did to its own directory scope is not inherited by
    # a second module either - `include_guard` above makes the lookup run exactly once, in whatever module happened to
    # be configured first. Both directories are therefore attached to the target.
    if(VN_OCCT_INCLUDE_DIR)
        target_include_directories(${target} SYSTEM PRIVATE ${VN_OCCT_INCLUDE_DIR})
    endif()
    if(VN_OCCT_LIBRARY_DIR)
        target_link_directories(${target} PRIVATE ${VN_OCCT_LIBRARY_DIR})
    endif()

    # TKPrim builds the parametric solids (BRepPrimAPI_*), which a caller that turns a geometry primitive into a
    # B-rep needs. TKG3d / TKGeomBase carry the geometry these operations touch (Geom_Surface, GeomLib::NormEstim in
    # tessellation, the GeomConvert approximation BRepBuilderAPI_GTransform produces); the boolean modules stay
    # behind BOOLEANS because they are the heaviest part.
    target_link_libraries(${target} PRIVATE
        TKDESTEP TKDEIGES TKXSBase TKBRep TKTopAlgo TKPrim TKG3d TKGeomBase TKernel TKMath)

    if("BOOLEANS" IN_LIST ARGN)
        target_link_libraries(${target} PRIVATE TKBO TKBool)
    endif()

    # Runtime: the modules are DLLs, so anything that links the backend has to find the OCCT DLLs when it runs. vcpkg's
    # applocal step only runs when the project is configured with its toolchain, which a build that merely points
    # OpenCASCADE_DIR at an installed tree is not, so the OCCT runtime is copied next to the target instead. All targets
    # share one output directory and the copy is `copy_if_different`, so calling this from several modules is cheap.
    if(VN_OCCT_BINARY_DIR)
        file(GLOB VN_OCCT_RUNTIME_DLLS "${VN_OCCT_BINARY_DIR}/TK*.dll")
        if(VN_OCCT_RUNTIME_DLLS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VN_OCCT_RUNTIME_DLLS} $<TARGET_FILE_DIR:${target}>
                COMMENT "Copying OpenCASCADE runtime next to ${target}")
        endif()
    endif()
endfunction()
