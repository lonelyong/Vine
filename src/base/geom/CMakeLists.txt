vn_add_library(GEOM_TARGET_NAME Geom)

# One module, two contracts. `sdk/vine/geom2d` and `sdk/vine/geom3d` are sibling namespace trees that share
# `sdk/vine/geom/geom_global.hpp` - one API macro, two namespace macros - so the include paths stay
# `<vine/geom2d/...>` and `<vine/geom3d/...>` and nothing downstream moved.
#
# The two trees must not include each other. Two CMake targets used to enforce that by refusing to link; one target
# cannot, so `scripts/check_geom_layering.py` does it now.
#
# The target is SHARED because the space side has explicit instantiations (`src/Instantiations.cpp`) and three
# non-template implementations. The planar side is header-only templates and simply travels in the same library.
target_link_libraries(${GEOM_TARGET_NAME} PUBLIC vn::Math vn::Core)
