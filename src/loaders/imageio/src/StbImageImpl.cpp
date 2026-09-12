// stb is compiled here and nowhere else.
//
// Kept in its own translation unit so the vendored headers can be given a
// relaxed warning profile without relaxing it for the codec that uses stb: the
// hand-written logic in ImageCodec.cpp stays under the project's strict flags.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "StbConfig.hpp"

#include <stb_image.h>
#include <stb_image_write.h>
