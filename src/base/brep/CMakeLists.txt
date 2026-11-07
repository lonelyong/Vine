# Brep — runtime B-rep operations.
#
# These are the operations a caller runs on a solid it already holds (tessellate
# it, build one from a primitive), as opposed to BrepIO, which reads and writes
# the files a solid comes from. The backend (OpenCASCADE) is optional and is
# linked PRIVATE when found, so the module builds and links without it:
# hasBackend() answers false and each operation returns a null result.
include(VineOpenCASCADE)

vn_add_library(BREP_TARGET_NAME Brep)

# Geometry is PUBLIC because the operations speak in Aabb / Solid types; Models because they hand back
# TriangleMesh values. Nothing else: a failure is a null value, so the module needs no error
# vocabulary and no IO dependency of its own.
target_link_libraries(${BREP_TARGET_NAME} PUBLIC vn::Geom vn::Models)

vn_link_opencascade(${BREP_TARGET_NAME} VN_BREP_HAS_OCCT)
