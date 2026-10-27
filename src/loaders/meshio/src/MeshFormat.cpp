#include <vine/meshio/MeshFormat.hpp>

#include <array>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <string>

#include "FormatTable.hpp"

VN_MESHIO_NS_BEGIN

namespace
{

/**
 * @brief One row of the format table: the format, its diagnostic name and the facts callers may query.
 *
 * The table is the only place a format is stated. MeshLoader::supportedExtensions(), formatFromPath() and formatInfo()
 * read it, so a format added here is readable, writable, offered by a file picker and reported by name in one edit.
 */
struct Entry
{
    MeshFormat     format;
    const char*    name;
    MeshFormatInfo info;
};

constexpr Entry kEntries[] = {
    { MeshFormat::Stl, "stl", { ".stl", true, true } },
    { MeshFormat::Obj, "obj", { ".obj", false, true } },
    { MeshFormat::Ply, "ply", { ".ply", true, true } },
    { MeshFormat::Gltf, "gltf", { ".gltf", false, false } },
    { MeshFormat::Glb, "glb", { ".glb", false, true } },
    { MeshFormat::Fbx, "fbx", { ".fbx", true, true } },
    { MeshFormat::Collada, "collada", { ".dae", false, true } },
    { MeshFormat::ThreeMf, "3mf", { ".3mf", false, false } },
};

constexpr MeshFormatInfo kUnknownInfo{ "", false, false };

/**
 * @brief Finds the row of a format.
 *
 * @param format Format to look up.
 * @return The row, or null for Unknown and out-of-range values.
 */
const Entry* findEntry(MeshFormat format) noexcept
{
    for (const Entry& entry : kEntries) {
        if (entry.format == format) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

namespace detail
{

std::span<const MeshFormat> allFormats() noexcept
{
    static const std::array<MeshFormat, std::size(kEntries)> formats = [] {
        std::array<MeshFormat, std::size(kEntries)> values{};
        for (std::size_t i = 0; i < std::size(kEntries); ++i) {
            values[i] = kEntries[i].format;
        }
        return values;
    }();
    return formats;
}

} // namespace detail

const char* formatName(MeshFormat format) noexcept
{
    const Entry* const entry = findEntry(format);
    return entry != nullptr ? entry->name : "Unknown";
}

MeshFormat formatFromPath(const std::filesystem::path& file_path) noexcept
{
    // A path whose extension cannot even be read names no format; the extension is ASCII, so folding it is all the
    // comparison needs and nothing has to be allocated beyond the extension itself.
    try {
        std::string extension = file_path.extension().string();
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        for (const Entry& entry : kEntries) {
            if (extension == entry.info.extension) {
                return entry.format;
            }
        }
    } catch (...) {
        return MeshFormat::Unknown;
    }
    return MeshFormat::Unknown;
}

const MeshFormatInfo& formatInfo(MeshFormat format) noexcept
{
    const Entry* const entry = findEntry(format);
    return entry != nullptr ? entry->info : kUnknownInfo;
}

VN_MESHIO_NS_END
