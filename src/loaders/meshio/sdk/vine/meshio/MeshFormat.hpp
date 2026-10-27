#pragma once

#include "meshio_global.hpp"

#include <cstdint>
#include <filesystem>

VN_MESHIO_NS_BEGIN

/**
 * @brief A mesh format this module reads and writes.
 *
 * The enum IS the contract: a value here is a format the loader opens and the exporter writes, and the extension that
 * selects it is stated by formatInfo(). A format the enum does not name is not read, even when the underlying library
 * could parse it - the alternative is two vocabularies that drift (the extension a reader accepts versus the writer
 * name a caller has to know).
 *
 * `Unknown` is the only value that is not a format: it is what formatFromPath() answers for a path this module does not
 * recognise, and what a default-constructed value holds, so "I do not know this file" can never be mistaken for a real
 * format.
 */
enum class MeshFormat : std::uint8_t
{
    Unknown = 0, ///< Not a format: an unrecognised extension, or no hint at all.
    Stl,         ///< Stereolithography (.stl); binary or text.
    Obj,         ///< Wavefront OBJ (.obj); text.
    Ply,         ///< Stanford polygon library (.ply); binary or text.
    Gltf,        ///< glTF 2.0 (.gltf); writes its buffer as a companion .bin, so it needs a path.
    Glb,         ///< glTF 2.0 binary container (.glb).
    Fbx,         ///< Autodesk FBX (.fbx); binary or text.
    Collada,     ///< COLLADA (.dae); text.
    ThreeMf,     ///< 3D Manufacturing Format (.3mf); one archive, written through a path.
};

/**
 * @brief The static facts a format has, as one value.
 *
 * @details
 * - `extension` is lowercase with the leading dot, and is the single spelling formatFromPath() matches against.
 * - `has_encoding` says Options::encoding is a CHOICE for this format; a format without it has one encoding, and the
 *   option is ignored.
 * - `streamable` says one self-contained file exists, which is what a stream or a data source can hold. A format that
 *   needs a second file for its normal form (.gltf's .bin) or cannot be produced without a path (.3mf) answers false.
 */
struct MeshFormatInfo
{
    const char* extension;    ///< Lowercase extension with the leading dot (".stl"); "" for Unknown.
    bool        has_encoding; ///< Whether Options::encoding selects a binary/text variant.
    bool        streamable;   ///< Whether one self-contained file can be produced.
};

/**
 * @brief Gets a stable, human-readable name for a format, for diagnostics.
 *
 * @param format Format to name.
 * @return The format's name ("stl", "collada", ...), or "Unknown" for MeshFormat::Unknown and out-of-range values.
 */
VN_MESHIO_API const char* formatName(MeshFormat format) noexcept;

/**
 * @brief Gets the format a path's extension names.
 *
 * The extension is a HINT: it says what the caller intends to open, and says nothing about what the bytes contain. The
 * comparison is case-insensitive, so "A.STL" and "a.stl" answer the same format.
 *
 * @param file_path Path to inspect; only its extension is used.
 * @return The format the extension names, or MeshFormat::Unknown when it names none this module handles.
 */
VN_MESHIO_API MeshFormat formatFromPath(const std::filesystem::path& file_path) noexcept;

/**
 * @brief Gets the static facts of a format.
 *
 * @param format Format to query.
 * @return The format's facts; a default-filled value (empty extension, no encoding choice, not streamable) for
 *         MeshFormat::Unknown and out-of-range values.
 */
VN_MESHIO_API const MeshFormatInfo& formatInfo(MeshFormat format) noexcept;

VN_MESHIO_NS_END
