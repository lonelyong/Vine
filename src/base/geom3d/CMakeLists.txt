vn_add_library(GEOM3D_TARGET_NAME Geom3D)

target_link_libraries(${GEOM3D_TARGET_NAME} PUBLIC vn::Math vn::Core)