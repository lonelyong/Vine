# Brep — runtime B-rep operations.
#
# These are the operations a caller runs on a solid it already holds (tessellate
# it, later build one from a primitive), as opposed to BrepIO, which reads and
# writes the files a solid comes from. The backend (OpenCASCADE) is optional and
# is linked PRIVATE when found, so the module builds and links without it and
# answers IoError::Unsupported at run time.
#
# The backend is not wired yet: the contract in Tessellation.hpp is implemented
# as a stub, and adding the body means including VineOpenCASCADE here and calling
# vn_link_opencascade(${BREP_TARGET_NAME} VN_BREP_HAS_OCCT).
vn_add_library(BREP_TARGET_NAME Brep)

# Geometry is PUBLIC because the operations speak in Shape/Mesh types; IOBase for Result/IoError, the module's
# single failure vocabulary.
target_link_libraries(${BREP_TARGET_NAME} PUBLIC vn::Geometry vn::IOBase)
