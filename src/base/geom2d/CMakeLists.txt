vn_add_library(GEOM2D_TARGET_NAME Geom2D)

# Header-only until the first 2D geometry type brings a translation unit: vn_add_library makes the target
# SHARED at that point, and this line must then say PUBLIC so the module builds against its own dependencies.
target_link_libraries(${GEOM2D_TARGET_NAME} INTERFACE vn::Math vn::Core)
