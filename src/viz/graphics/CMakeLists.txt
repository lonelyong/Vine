# ONE MODULE, TWO IMAGES OF THE SAME OBJECTS.
#
# The object set is what COMPILES the module; `vn::Graphics` is the image the SDK ships and every other binary
# links. The module's own test links the object set instead: it drives the private half (the frame pump, the
# publication store, the scene rows, the draw-list table), so it has to call those definitions - and it may not
# hold a second copy of them while also loading the library, because two definitions of one class in one process
# is an out-of-line ODR violation that AddressSanitizer reports before a case runs. One set of objects, one
# definition per process, on every platform. See vn_add_library's note and
# ADR-2026-11-27-a-module-test-links-the-modules-objects.
set(GRAPHICS_OBJECTS GraphicsObjects)
vn_add_library(GRAPHICS_TARGET_NAME Graphics OBJECT_TARGET ${GRAPHICS_OBJECTS})

# WHAT THE MODULE'S SOURCES ARE COMPILED AGAINST BELONGS TO BOTH IMAGES: the object set compiles, and the shared
# library has to state the same facts to its consumers. One loop, so the two cannot drift.
foreach(vn_image ${GRAPHICS_TARGET_NAME} ${GRAPHICS_OBJECTS})
    # The version a backend module reports as "built against" (RenderBackendAbi::sdk_version).
    # PUBLIC: a backend's own translation unit expands VN_DECLARE_RENDER_BACKEND_ABI(), and what it must report is the SDK
    # it was compiled against - not the host it happens to be loaded into.
    target_compile_definitions(${vn_image} PUBLIC VN_GRAPHICS_VERSION="${PROJECT_VERSION}")
    target_link_libraries(${vn_image} PUBLIC vn::Core vn::Global vn::Geom vn::Models vn::Window vn::Imaging)
endforeach()

# Source files (src/*.cpp, incl. the HUD overlays AxisGizmo.cpp / FpsOverlay.cpp and the shared
# axes-marker builder AxesMarker.cpp) are collected from src/ by vn_add_library's glob, which has no
# CONFIGURE_DEPENDS: add a new file -> reconfigure.
#
# The GLSL programs live in shaders/ (outside the globbed directories) and reach
# the library as the generated vine/graphics/EmbeddedShaders.hpp. Wired to the OBJECT SET: that is the target that
# compiles the translation units including the header.
vn_use_embedded_shaders(${GRAPHICS_OBJECTS} OUTPUT vine/graphics/EmbeddedShaders.hpp SOURCES)

# One module, divided by what crosses the DLL boundary - not by who happens to read it:
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
# The division is a RULE about the export table, not about who reads a header: the module's own test reads src/ and
# still holds no second definition, because it links the object set above instead of the DLL.
#
# What the SDK may not do is a RULE, not a directory: it must not include a particular backend
# (check_include_hygiene.py), it must name no render API (check_vk_exposure.py), and anything exported
# must be DECLARED in sdk/ (check_export_annotations.py).
#
# One target because every part of graphics depends on this module, and because a backend then depends on
# exactly one thing: vn::Graphics.
