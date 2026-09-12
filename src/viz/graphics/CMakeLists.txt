v_add_library(GRAPHICS_TARGET_NAME Graphics)

# Source files (src/*.cpp, incl. the HUD overlays AxisGizmo.cpp / FpsOverlay.cpp)
# are collected from src/ by v_add_library's glob; add a new file -> reconfigure.

target_link_libraries(${GRAPHICS_TARGET_NAME} PUBLIC vi::Core vi::Global vi::Geometry vi::Window vi::Imaging)
