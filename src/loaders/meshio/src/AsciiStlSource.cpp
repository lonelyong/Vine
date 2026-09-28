#include <vine/meshio/AsciiStlSource.hpp>

#include <string_view>
#include <utility>

#include "MeshTextWriter.hpp"

VN_MESHIO_NS_BEGIN

namespace
{

using Mesh       = vn::geometry::Mesh;
using detail::kNoRoom;

/// The name an ASCII STL carries in its "solid" line; nothing reads it, but the format asks for one.
constexpr std::string_view kSolidName = "vine";

/// Enough for a facet: three coordinates plus a normal, each at most about twenty characters.
constexpr std::size_t kFacetSize = 384;

/**
 * @brief Appends one coordinate to the line being built.
 *
 * @param out The line being built.
 * @param at Offset to append at.
 * @param value The coordinate.
 * @return The offset after the text, or kNoRoom when it does not fit.
 */
std::size_t appendCoordinate(std::span<char> out, std::size_t at, float value)
{
    return detail::appendFloat(out, at, value);
}

/**
 * @brief Appends one corner to the line being built.
 *
 * @param out The line being built.
 * @param at Offset to append at.
 * @param vertex The corner to append.
 * @return The offset after the text, or kNoRoom when it does not fit.
 */
std::size_t appendVertex(std::span<char> out, std::size_t at, const float (&vertex)[3])
{
    static constexpr std::string_view kIndent = "    vertex ";
    at                                        = detail::appendText(out, at, kIndent);
    at                                        = appendCoordinate(out, at, vertex[0]);
    at                                        = detail::appendText(out, at, " ");
    at                                        = appendCoordinate(out, at, vertex[1]);
    at                                        = detail::appendText(out, at, " ");
    at                                        = appendCoordinate(out, at, vertex[2]);
    return detail::appendText(out, at, "\n");
}

/**
 * @brief Writes one facet, the whole block from "facet normal" to "endfacet".
 *
 * @param out The line buffer.
 * @param normal The facet's unit normal.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @return The bytes written, or 0 when they do not fit the buffer.
 */
std::size_t writeFacet(std::span<char> out, const float (&normal)[3], const float (&a)[3], const float (&b)[3],
                       const float (&c)[3])
{
    std::size_t at = detail::appendText(out, 0, "facet normal ");
    at             = appendCoordinate(out, at, normal[0]);
    at             = detail::appendText(out, at, " ");
    at             = appendCoordinate(out, at, normal[1]);
    at             = detail::appendText(out, at, " ");
    at             = appendCoordinate(out, at, normal[2]);
    at             = detail::appendText(out, at, "\n  outer loop\n");
    at             = appendVertex(out, at, a);
    at             = appendVertex(out, at, b);
    at             = appendVertex(out, at, c);
    at             = detail::appendText(out, at, "  endloop\nendfacet\n");
    return at == kNoRoom ? 0 : at;
}

/**
 * @brief Writes the "solid" line or the "endsolid" one.
 *
 * @param out The line buffer.
 * @param keyword The keyword the line starts with.
 * @return The bytes written, or 0 when they do not fit the buffer.
 */
std::size_t writeWrapper(std::span<char> out, std::string_view keyword)
{
    std::size_t at = detail::appendText(out, 0, keyword);
    at             = detail::appendText(out, at, " ");
    at             = detail::appendText(out, at, kSolidName);
    at             = detail::appendText(out, at, "\n");
    return at == kNoRoom ? 0 : at;
}

} // namespace

AsciiStlSource::AsciiStlSource(vn::intrusive_ptr<const Mesh> mesh, double scale) : MeshSource(std::move(mesh), scale)
{
    // No length is stated: a text format only knows its size once it is laid out, and the pull can end on its own.
}

AsciiStlSource::~AsciiStlSource() = default;

AsciiStlSource::AsciiStlSource(AsciiStlSource&& other) noexcept = default;

AsciiStlSource& AsciiStlSource::operator=(AsciiStlSource&& other) noexcept = default;

std::uint64_t AsciiStlSource::recordCount() const
{
    return triangleCount() + 2; // the leading "solid", one record per facet, the trailing "endsolid"
}

std::size_t AsciiStlSource::maxRecordSize() const
{
    return kFacetSize;
}

std::size_t AsciiStlSource::renderRecord(std::uint64_t index, std::span<char> out)
{
    if (index == 0) {
        return writeWrapper(out, "solid");
    }
    if (index > triangleCount()) {
        return writeWrapper(out, "endsolid");
    }

    float a[3]{};
    float b[3]{};
    float c[3]{};
    if (!triangleAt(index - 1, a, b, c)) {
        return 0; // the mesh stopped holding a triangle it counted
    }

    float normal[3]{};
    facetNormal(a, b, c, normal);
    return writeFacet(out, normal, a, b, c);
}

VN_MESHIO_NS_END
