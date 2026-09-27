vn_add_library(GRAPHICS_TARGET_NAME Graphics)

# Source files (src/*.cpp, incl. the HUD overlays AxisGizmo.cpp / FpsOverlay.cpp)
# are collected from src/ by vn_add_library's glob; add a new file -> reconfigure.
#
# The GLSL programs live in shaders/ (outside the globbed directories) and reach
# the library as the generated vine/graphics/EmbeddedShaders.hpp.
vn_use_embedded_shaders(${GRAPHICS_TARGET_NAME} OUTPUT vine/graphics/EmbeddedShaders.hpp SOURCES)

# Two layers inside one module and one target (see .ai/design/graphics-layering.md), divided by what
# crosses the DLL boundary - not by who happens to read them:
#
#   sdk/vine/graphics/…    everything EXPORTED, and therefore installed: the host half (scene, camera,
#                          RenderEngine, RenderPass, RenderTarget, the RenderBackend contract) and
#                          the backend half (sdk/vine/graphics/backend/: one flat directory - the frame
#                          discipline, the device facts, the byte arenas and the evidence fixtures).
#   src/…                  what only this DLL uses: implementations, plus private headers sitting NEXT
#                          TO their .cpp (the appfw convention, included with "..."). Those are on no
#                          include root, so nothing outside this module can include them at all.
#
# Who may include what is a RULE, not a directory: a host must not include sdk/vine/graphics/backend/
# (check_include_hygiene.py), the backend half must not include any particular backend (same script),
# and anything exported must be DECLARED in sdk/ (check_export_annotations.py).
#
# One target because neither half can exist without this module - they are layers of graphics, not a
# second component - and because a backend then depends on exactly one thing: vn::Graphics.
target_link_libraries(${GRAPHICS_TARGET_NAME} PUBLIC vn::Core vn::Global vn::Geometry vn::Window vn::Imaging)
