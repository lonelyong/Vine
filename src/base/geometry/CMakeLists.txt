vn_add_library(GEOMETRY_TARGET_NAME Geometry)

target_link_libraries(${GEOMETRY_TARGET_NAME} PUBLIC vn::Math vn::Core)