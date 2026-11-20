vn_add_library(GRAPHICS_TARGET_NAME Graphics)

# Source files (src/*.cpp, incl. the HUD overlays AxisGizmo.cpp / FpsOverlay.cpp and the shared
# axes-marker builder AxesMarker.cpp) are collected from src/ by vn_add_library's glob, which has no
# CONFIGURE_DEPENDS: add a new file -> reconfigure.
#
# The GLSL programs live in shaders/ (outside the globbed directories) and reach
# the library as the generated vine/graphics/EmbeddedShaders.hpp.
vn_use_embedded_shaders(${GRAPHICS_TARGET_NAME} OUTPUT vine/graphics/EmbeddedShaders.hpp SOURCES)

# One module, one target, divided by what crosses the DLL boundary - not by who happens to read it:
#
#   sdk/vine/graphics/…    everything EXPORTED, and therefore installed: the scene, camera, RenderEngine,
#                          RenderPass, RenderTarget and the RenderBackend contract, plus the frame
#                          mechanisms the backends share (the frame gate, the device clock and its frame
#                          timeline, retirement, readback, the format/compatibility keys, the target plan
#                          and the refusal draft). What only one backend needs lives in that backend.
#   src/…                  what only this DLL uses: implementations, plus private headers sitting NEXT
#                          TO their .cpp (the appfw convention, included with "..."). Those are on no
#                          include root, so nothing outside this module can include them at all.
#
# What the SDK may not do is a RULE, not a directory: it must not include a particular backend
# (check_include_hygiene.py), it must name no render API (check_vk_exposure.py), and anything exported
# must be DECLARED in sdk/ (check_export_annotations.py).
#
# One target because every part of graphics depends on this module, and because a backend then depends on
# exactly one thing: vn::Graphics.
target_link_libraries(${GRAPHICS_TARGET_NAME} PUBLIC vn::Core vn::Global vn::Geom vn::Models vn::Window vn::Imaging)
