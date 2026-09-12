# stb — single-file public-domain image loader / writer (stb_image v2.30,
# stb_image_write v1.16), vendored.
#
# Vendored rather than fetched: upstream (nothings/stb) ships no CMakeLists and
# no versioned releases, so a FetchContent of it would pull the whole repository
# (all its headers, docs and test images) just to get two files, and would need
# a hand-built target anyway. The two headers are self-contained and are meant
# to be dropped into a tree like this one.
#
# Header-only: an INTERFACE target carries the include directory, so the
# implementation TU lives in the consumer (see src/loaders/imageio) and stb is
# compiled exactly once.
add_library(stb INTERFACE)
add_library(tp::stb ALIAS stb)

set_target_properties(stb PROPERTIES FOLDER third_party)

target_sources(stb INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/stb_image.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/stb_image_write.h"
)

target_include_directories(stb INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
