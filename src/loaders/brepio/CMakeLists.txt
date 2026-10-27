# BrepIO — boundary-representation import/export (STEP, IGES).
#
# Split out of the former `modelio`: this half has NO third-party dependency, so
# it stays cheap to build and usable without assimp. That is the point of the
# split — the two halves only shared a name.
#
# The STEP/IGES backend (OpenCASCADE) is linked PRIVATE when it is found, so mesh-only consumers keep not paying for it.
# Without it the module answers IoError::Unsupported and supportedExtensions() is empty.
vn_add_library(BREPIO_TARGET_NAME BrepIO)

# IOBase is PUBLIC because BrepExporter::createSource hands a vn::io::DataSource to the caller.
target_link_libraries(${BREPIO_TARGET_NAME} PUBLIC vn::Runtime vn::Geometry vn::Crypto vn::IOBase)

find_package(OpenCASCADE CONFIG QUIET)
if(OpenCASCADE_FOUND)
    # OCCT 8 split the old TKSTEP/TKIGES modules: STEP is TKDESTEP, IGES is TKDEIGES. The imported targets are bare
    # names, not `OpenCASCADE::`-prefixed ones.
    message(STATUS "BrepIO: OpenCASCADE found - STEP/IGES/BREP enabled")
    target_compile_definitions(${BREPIO_TARGET_NAME} PRIVATE VN_BREPIO_HAS_OCCT)
    target_link_libraries(${BREPIO_TARGET_NAME} PRIVATE
        TKDESTEP TKDEIGES TKXSBase TKBRep TKTopAlgo TKernel TKMath)
else()
    message(STATUS "BrepIO: OpenCASCADE not found - STEP/IGES/BREP disabled")
endif()
