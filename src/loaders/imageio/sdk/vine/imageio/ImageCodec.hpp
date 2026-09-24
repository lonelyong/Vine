#pragma once

#include "imageio_global.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/intrusive_ptr.hpp>

VN_IMAGEIO_NS_BEGIN

/**
 * @brief A container format an image file can be stored in.
 *
 * The container is what wraps the pixels; the PIXEL layout inside it is an `imaging::PixelFormat` and is
 * chosen by the caller, not fixed by the container.
 * A PNG and a TGA can both hold the same RGBA8 pixels, and decoding either into the same format gives the
 * same `Image`.
 *
 * `Unknown` is the only value that is not a format: it is what `formatFromPath()` answers for a path this
 * module does not recognise, and what a default-constructed value has, so a caller cannot accidentally
 * treat "I don't know this file" as a real format.
 */
enum class ImageFileFormat : std::uint8_t {
    Unknown = 0, ///< Not a format, or a container this module does not handle.
    Png,         ///< PNG. Read and written.
    Jpeg,        ///< JPEG. Read only (writing it is lossy and needs a quality parameter this API has no place for).
    Bmp,         ///< BMP. Read and written.
    Tga,         ///< TGA. Read and written.
};

/**
 * @brief Gets a stable, human-readable name for a container format, for diagnostics.
 *
 * @param format Format to name.
 * @return The format's name, or `"Unknown"` for `ImageFileFormat::Unknown` and any out-of-range value.
 */
VN_IMAGEIO_API const char* formatName(ImageFileFormat format) noexcept;

/**
 * @brief Gets the container format a path's extension names.
 *
 * The extension is a HINT, not evidence: it says what the caller intends to write, and says nothing about
 * what an existing file actually contains. Decoding never consults it — the decoder reads the bytes and
 * finds the container itself — so a file whose name lies still decodes correctly.
 *
 * @param path Path to inspect; only its extension is used.
 * @return The format the extension names, or `ImageFileFormat::Unknown` when it names none this module
 *         handles.
 */
VN_IMAGEIO_API ImageFileFormat formatFromPath(const std::filesystem::path& path);

/**
 * @brief States whether this module can decode a container format.
 *
 * @param format Format to query.
 * @return true when `decodeImage()` accepts a file in this container.
 */
VN_IMAGEIO_API bool canRead(ImageFileFormat format) noexcept;

/**
 * @brief States whether this module can encode a container format.
 *
 * @param format Format to query.
 * @return true when `encodeImage()` can produce this container.
 */
VN_IMAGEIO_API bool canWrite(ImageFileFormat format) noexcept;

/**
 * @brief Decodes an image from bytes already in memory.
 *
 * The container is detected from the bytes, so the same call handles every readable format and a wrong file
 * extension cannot mislead it.
 *
 * The requested `format` is what the caller needs, not what the file holds: a JPEG storing three channels
 * decoded as `Rgba8Unorm` comes back as RGBA8, because the conversion is a channel shuffle the decoder
 * already has to do. This is where the "decoded pixels are RGB8 but no GPU has a 24-bit texture format"
 * problem is solved — the caller asks for a layout it can actually upload.
 *
 * WHAT THE CHANNELS MEAN. One channel is the LUMA of the source (Rec.601 weights, no rounding), so
 * `R8Unorm` here is greyscale rather than a copy of the source's red channel. Three and four channels are
 * RGB and RGBA. TWO channels are refused: a file's two channels mean grey + alpha, and `Rg8Unorm` means red
 * + green, so answering it would put luma in a channel the caller called red.
 *
 * The result has exactly ONE mip level: a container stores one image, and generating the chain above it is
 * a separate operation this module does not perform.
 *
 * @param bytes  Encoded file bytes.
 * @param format Pixel layout to decode into; must be one of the 8-bit colour layouts.
 * @return The decoded image, never null.
 * @throws std::invalid_argument if @p format is not an 8-bit colour layout, or @p bytes is empty.
 * @throws std::runtime_error if the bytes are not a readable image, carrying the decoder's own reason.
 */
VN_IMAGEIO_API intrusive_ptr<imaging::Image> decodeImage(std::span<const std::byte> bytes, imaging::PixelFormat format);

/**
 * @brief Decodes an image from a file.
 *
 * @param path   File to read.
 * @param format Pixel layout to decode into; must be one of the 8-bit colour layouts.
 * @return The decoded image, never null.
 * @throws std::invalid_argument if @p format is not an 8-bit colour layout.
 * @throws std::runtime_error if the file cannot be read or does not hold a readable image.
 */
VN_IMAGEIO_API intrusive_ptr<imaging::Image> loadImage(const std::filesystem::path& path, imaging::PixelFormat format);

/**
 * @brief Encodes an image into bytes in the given container.
 *
 * Only the BASE mip level is written: PNG, BMP and TGA hold a single image and cannot carry a chain, so a
 * multi-level image is not rejected but is written as its base level rather than silently flattened.
 *
 * The channel layouts a container can store are the same ones `decodeImage()` can produce, with one
 * asymmetry: BMP has no two-channel form in its writer, and a one-channel image is written as greyscale by
 * replicating the channel into R, G and B.
 *
 * @param image  Image to encode.
 * @param format Container to produce; must be one this module can write.
 * @return The encoded bytes, never empty.
 * @throws std::invalid_argument if @p format cannot be written, or the image's pixel layout is not one of
 *         the 8-bit colour layouts.
 * @throws std::runtime_error if the encoder fails.
 */
VN_IMAGEIO_API std::vector<std::byte> encodeImage(const imaging::Image& image, ImageFileFormat format);

/**
 * @brief Encodes an image and writes it to a file.
 *
 * The format is taken as given rather than guessed from the extension: guessing would let a typo in a file
 * name silently produce a container the caller did not ask for.
 *
 * @param path   File to write; an existing file is replaced.
 * @param image  Image to encode.
 * @param format Container to produce; must be one this module can write.
 * @throws std::invalid_argument if @p format cannot be written, or the image's pixel layout is not one of
 *         the 8-bit colour layouts.
 * @throws std::runtime_error if the file cannot be written or the encoder fails.
 */
VN_IMAGEIO_API void saveImage(const std::filesystem::path& path, const imaging::Image& image, ImageFileFormat format);

VN_IMAGEIO_NS_END
