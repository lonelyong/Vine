#include <vine/meshio/ObjSource.hpp>

#include <string_view>
#include <utility>

#include "MeshTextWriter.hpp"

VN_MESHIO_NS_BEGIN

namespace
{

using Mesh       = vn::geometry::Mesh;
using detail::kNoRoom;

/// Enough for a face naming three vertices and three normals, the longest line this format writes.
constexpr std::size_t kFaceSize = 512;

/**
 * @brief Appends three coordinates after a keyword.
 *
 * @param out The line being built.
 * @param keyword The keyword the line starts with, e.g. "v".
 * @param values The three coordinates.
 * @return The offset after the text, or kNoRoom when it does not fit.
 */
std::size_t writeVector(std::span<char> out, std::string_view keyword, const float (&values)[3])
{
    std::size_t at = detail::appendText(out, 0, keyword);
    for (const float value : values) {
        at = detail::appendText(out, at, " ");
        at = detail::appendFloat(out, at, value);
    }
    return detail::appendText(out, at, "\n");
}

/**
 * @brief Appends one face corner: a vertex index, and a normal index when the mesh has normals.
 *
 * OBJ counts from one, and the normal index is the vertex index because this source writes one "vn" per vertex.
 *
 * @param out The line being built.
 * @param at Offset to append at.
 * @param vertex The corner's vertex index, counting from zero.
 * @param normals Whether the corner also names a normal.
 * @return The offset after the text, or kNoRoom when it does not fit.
 */
std::size_t writeCorner(std::span<char> out, std::size_t at, std::uint32_t vertex, bool normals)
{
    const auto index = static_cast<std::uint64_t>(vertex) + 1;
    at               = detail::appendText(out, at, " ");
    at               = detail::appendInteger(out, at, index);
    if (normals) {
        at = detail::appendText(out, at, "//");
        at = detail::appendInteger(out, at, index);
    }
    return at;
}

} // namespace

ObjSource::ObjSource(vn::intrusive_ptr<const Mesh> mesh, double scale) : MeshSource(std::move(mesh), scale)
{
    // No length is stated: a text format only knows its size once it is laid out, and the pull can end on its own. The
    // cost is on the backend that stores it (a ZIP writes such an entry with a zip64 header).
}

ObjSource::~ObjSource() = default;

ObjSource::ObjSource(ObjSource&& other) noexcept = default;

ObjSource& ObjSource::operator=(ObjSource&& other) noexcept = default;

std::uint64_t ObjSource::recordCount() const
{
    const std::uint64_t vertices = vertexCount();
    const std::uint64_t normals  = hasNormals() ? vertices : 0;
    return vertices + normals + triangleCount();
}

std::size_t ObjSource::maxRecordSize() const
{
    return kFaceSize;
}

std::size_t ObjSource::renderRecord(std::uint64_t index, std::span<char> out)
{
    const std::uint64_t vertices = vertexCount();
    if (index < vertices) {
        float position[3]{};
        if (!vertexAt(index, position)) {
            return 0;
        }
        return writeVector(out, "v", position);
    }

    const std::uint64_t normals = hasNormals() ? vertices : 0;
    if (index < vertices + normals) {
        float normal[3]{};
        if (!normalAt(index - vertices, normal)) {
            return 0;
        }
        return writeVector(out, "vn", normal);
    }

    std::uint32_t corners[3]{};
    if (!cornerAt(index - vertices - normals, corners)) {
        return 0; // the mesh stopped holding a triangle it counted
    }

    std::size_t at = detail::appendText(out, 0, "f");
    for (const std::uint32_t corner : corners) {
        at = writeCorner(out, at, corner, normals != 0);
    }
    at = detail::appendText(out, at, "\n");
    return at == kNoRoom ? 0 : at;
}

VN_MESHIO_NS_END
