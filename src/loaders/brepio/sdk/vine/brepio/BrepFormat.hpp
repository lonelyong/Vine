#pragma once

#include "brepio_global.hpp"

#include <cstdint>
#include <filesystem>

VN_BREPIO_NS_BEGIN

/**
 * @brief A B-rep format this module reads and writes.
 *
 * The enum is the contract, the way meshio's MeshFormat is: a value here is a format both directions speak, and the
 * extension that selects it is what formatFromPath() matches.
 *
 * `Unknown` is the only value that is not a format: it is what formatFromPath() answers for a path this module does not
 * recognise, and what a default-constructed value holds.
 */
enum class BrepFormat : std::uint8_t
{
    Unknown = 0, // Not a format: an unrecognised extension, or no hint at all.
    Step,        // STEP (.stp, .step); text.
    Iges,        // IGES (.igs, .iges); text.
    Brep,        // OpenCASCADE's native B-Rep (.brep); binary.
};

/**
 * @brief Gets a stable, human-readable name for a format, for diagnostics.
 *
 * @param format Format to name.
 * @return The format's name ("step", "iges", "brep"), or "Unknown" for BrepFormat::Unknown and out-of-range values.
 */
VN_BREPIO_API const char* formatName(BrepFormat format) noexcept;

/**
 * @brief Gets the format a path's extension names.
 *
 * The comparison is case-insensitive, so "A.STEP" and "a.step" answer the same format.
 *
 * @param file_path Path to inspect; only its extension is used.
 * @return The format the extension names, or BrepFormat::Unknown when it names none this module handles.
 */
VN_BREPIO_API BrepFormat formatFromPath(const std::filesystem::path& file_path) noexcept;

VN_BREPIO_NS_END
