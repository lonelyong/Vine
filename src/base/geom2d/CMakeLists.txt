vn_add_library(GEOM2D_TARGET_NAME Geom2D)

# The 2D curves are header-only templates, so this stays an INTERFACE target: there is nothing to link until a
# non-template implementation needs its own translation unit, at which point this line becomes PUBLIC.
target_link_libraries(${GEOM2D_TARGET_NAME} INTERFACE vn::Math vn::Core)
