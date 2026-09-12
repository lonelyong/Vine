# ImageIO — image container decoding/encoding, backed by vendored stb.
#
# Sits in loaders/ next to MeshIO, for the same reason: the DATA type lives in
# a base module (geometry::Mesh, imaging::Image) and the third-party codec that
# fills it from a file lives here, linked PRIVATE so this module is the only
# thing that carries stb.
v_add_library(IMAGEIO_TARGET_NAME ImageIO)

target_link_libraries(${IMAGEIO_TARGET_NAME} PUBLIC vi::Imaging)
target_link_libraries(${IMAGEIO_TARGET_NAME} PRIVATE tp::stb)
