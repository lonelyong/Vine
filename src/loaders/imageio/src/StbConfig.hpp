#pragma once

// Included before <stb_image.h> / <stb_image_write.h> in EVERY translation unit
// of this module, so the declarations ImageCodec.cpp compiles against match the
// ones StbImageImpl.cpp defined. Defining this differently in the two files
// would be a silent mismatch.
//
// STBI_ONLY_* trims the decoder to the containers this module advertises, which
// cuts both compile time and the amount of third-party code in the binary.
// STBI_NO_STDIO / STBI_WRITE_NO_STDIO remove the path-based entry points: the
// codec reads and writes bytes itself (see ImageCodec.cpp), which keeps paths
// portable and makes the whole codec testable without touching a file.

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STBI_WRITE_NO_STDIO
