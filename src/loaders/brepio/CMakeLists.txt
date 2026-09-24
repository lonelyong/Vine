# BrepIO — boundary-representation import/export (STEP, IGES).
#
# Split out of the former `modelio`: this half has NO third-party dependency, so
# it stays cheap to build and usable without assimp. That is the point of the
# split — the two halves only shared a name.
#
# The STEP/IGES backend (OpenCASCADE) is NOT wired in yet: BrepLoader::load()
# returns null and says so. When it lands it will be linked PRIVATE here, so
# mesh-only consumers keep not paying for it.
vn_add_library(BREPIO_TARGET_NAME BrepIO)

target_link_libraries(${BREPIO_TARGET_NAME} PUBLIC vn::Runtime vn::Geometry vn::Crypto)
