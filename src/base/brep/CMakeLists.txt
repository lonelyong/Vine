# Brep — runtime B-rep operations.
#
# These are the operations a caller runs on a solid it already holds (tessellate
# it, later build one from a primitive), as opposed to BrepIO, which reads and
# writes the files a solid comes from. The backend (OpenCASCADE) is optional and
# is linked PRIVATE when found, so the module builds and links without it:
# tessellationSupported() answers false and tessellate() returns a null mesh.
include(VineOpenCASCADE)

vn_add_library(BREP_TARGET_NAME Brep)

# Geometry is PUBLIC because the operations speak in Shape/Mesh types. Nothing else: a failure is a null value, so
# the module needs no error vocabulary and no IO dependency of its own.
target_link_libraries(${BREP_TARGET_NAME} PUBLIC vn::Geometry)

vn_link_opencascade(${BREP_TARGET_NAME} VN_BREP_HAS_OCCT)
