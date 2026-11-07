# BrepIO — boundary-representation import/export (STEP, IGES).
#
# Split out of the former `modelio`: this half has NO third-party dependency, so
# it stays cheap to build and usable without assimp. That is the point of the
# split — the two halves only shared a name.
#
# The STEP/IGES backend (OpenCASCADE) is linked PRIVATE when it is found, so mesh-only consumers keep not paying for it.
# Without it the module answers IoError::Unsupported and supportedExtensions() is empty.
include(VineOpenCASCADE)

vn_add_library(BREPIO_TARGET_NAME BrepIO)

# IOBase is PUBLIC because BrepExporter::createSource hands a vn::io::DataSource to the caller.
target_link_libraries(${BREPIO_TARGET_NAME} PUBLIC vn::Runtime vn::Geom vn::Models vn::Crypto vn::IOBase)

# STEP/IGES need the OpenCASCADE backend. The probe is shared with the brep module (cmake/VineOpenCASCADE.cmake)
# and links PRIVATE, so mesh-only consumers keep not paying for it; without it the module answers
# IoError::Unsupported and supportedExtensions() is empty.
vn_link_opencascade(${BREPIO_TARGET_NAME} VN_BREPIO_HAS_OCCT)
