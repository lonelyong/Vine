vn_add_library(GRAPHICS_TARGET_NAME Graphics)

# Source files (src/*.cpp, incl. the HUD overlays AxisGizmo.cpp / FpsOverlay.cpp)
# are collected from src/ by vn_add_library's glob; add a new file -> reconfigure.
#
# The GLSL programs live in shaders/ (outside the globbed directories) and reach
# the library as the generated vine/graphics/EmbeddedShaders.hpp.
vn_use_embedded_shaders(${GRAPHICS_TARGET_NAME} OUTPUT vine/graphics/EmbeddedShaders.hpp SOURCES)

target_link_libraries(${GRAPHICS_TARGET_NAME} PUBLIC vn::Core vn::Global vn::Geometry vn::Window vn::Imaging)
