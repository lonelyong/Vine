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
    # TKPrim builds the parametric solids (BRepPrimAPI_*), which a caller that turns a geometry primitive into a
    # B-rep needs. TKG3d / TKGeomBase carry the geometry these operations touch (Geom_Surface, GeomLib::NormEstim in
    # tessellation, the GeomConvert approximation BRepBuilderAPI_GTransform produces); the boolean modules stay
    # behind BOOLEANS because they are the heaviest part.
    target_link_libraries(${target} PRIVATE
        TKDESTEP TKDEIGES TKXSBase TKBRep TKTopAlgo TKPrim TKG3d TKGeomBase TKernel TKMath)

    if("BOOLEANS" IN_LIST ARGN)
        target_link_libraries(${target} PRIVATE TKBO TKBool)
    endif()
endfunction()
